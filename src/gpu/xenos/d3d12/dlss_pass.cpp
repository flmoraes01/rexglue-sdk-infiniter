/**
 * @file        gpu/xenos/d3d12/dlss_pass.cpp
 * @brief       NVIDIA DLSS (DLAA mode) applied in place to the guest's scene render target.
 */

#include "gpu/xenos/d3d12/dlss_pass.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <cstring>
#include <string>

#include <rex/logging/log_category.h>

#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/cas_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/dlss_inputs_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/dlss_nr_compose_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/smaa_blend_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/ssgi_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/smaa_edges_cs.h"
#include "gpu/xenos/d3d12/shaders/bytecode/d3d12_5_1/smaa_weights_cs.h"
#include "thirdparty/smaa/AreaTex.h"
#include "thirdparty/smaa/SearchTex.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

#if REX_FFX
#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#endif

#if REX_DLSS
// NVSDK_NGX_FAILED compares an unsigned mask with the result enum.
#pragma clang diagnostic ignored "-Wsign-compare"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

namespace rex::gpu::d3d12 {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct InputsConstants {
  f32 reproject[16];
  u32 depth_origin[2];
  u32 size[2];
};

class Barriers {
 public:
  void Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after || count_ >= std::size(barriers_)) {
      return;
    }
    D3D12_RESOURCE_BARRIER& barrier = barriers_[count_++];
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
  }
  void Submit(ID3D12GraphicsCommandList* command_list) {
    if (count_) {
      command_list->ResourceBarrier(count_, barriers_);
      count_ = 0;
    }
  }

 private:
  D3D12_RESOURCE_BARRIER barriers_[8];
  UINT count_ = 0;
};

DXGI_FORMAT DepthSRVFormat(DXGI_FORMAT resource_format) {
  switch (resource_format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:  // depth the texture cache decoded from a guest resolve
      return DXGI_FORMAT_R32_FLOAT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT resource_format) {
  switch (resource_format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:  // HDR scene colour loaded by the texture cache
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
      return resource_format;
  }
}

}  // namespace

DlssPass::~DlssPass() {
  Shutdown();
}

bool DlssPass::Initialize(ID3D12Device* device) {
  Shutdown();
  device_ = device;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 2;
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = sizeof(InputsConstants) / sizeof(u32);
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (u32 i = 0; i < 2; ++i) {
    parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = UINT(std::size(parameters));
  root_desc.pParameters = parameters;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    REXGPU_WARN("DLSS: cannot create the inputs root signature");
    return false;
  }
  inputs_pipeline_.Attach(objects::CreateComputePipeline(device, kDlssInputsCS,
                                                         sizeof(kDlssInputsCS),
                                                         root_signature_.Get()));
  compose_pipeline_.Attach(objects::CreateComputePipeline(device, kDlssNrComposeCS,
                                                          sizeof(kDlssNrComposeCS),
                                                          root_signature_.Get()));
  cas_pipeline_.Attach(objects::CreateComputePipeline(device, kCasCS, sizeof(kCasCS),
                                                      root_signature_.Get()));
  if (!inputs_pipeline_ || !compose_pipeline_ || !cas_pipeline_ || !InitializeSmaa() ||
      !InitializeGi()) {
    REXGPU_WARN("DLSS: cannot create the compute pipelines");
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kSlotCount;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_)))) {
    REXGPU_WARN("DLSS: cannot create the descriptor heap");
    return false;
  }
  descriptor_size_ =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  // The pass itself (SMAA) works from here on; DLSS and Neural Rendering need NGX as well.
  available_ = true;
#if REX_DLSS
  // NGX writes its logs to the application data path; the working directory is enough.
  NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
      "5a1d8d3e-7c2b-4f0e-9d61-1d2f0e3c4b5a", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L".", device);
  if (NVSDK_NGX_FAILED(result)) {
    REXGPU_WARN("DLSS: NGX did not initialize (0x{:08X}), only SMAA is available",
                static_cast<u32>(result));
    return true;
  }
  ngx_initialized_ = true;
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_);
  if (NVSDK_NGX_FAILED(result) || !parameters_) {
    REXGPU_WARN("DLSS: no NGX capability parameters (0x{:08X}), only SMAA is available",
                static_cast<u32>(result));
    return true;
  }
  int supported = 0;
  if (NVSDK_NGX_FAILED(
          parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported)) ||
      !supported) {
    REXGPU_WARN("DLSS: this GPU or driver does not offer DLSS (is nvngx_dlss.dll next to the "
                "exe?), only SMAA is available");
    return true;
  }
  dlss_supported_ = true;
  REXGPU_INFO("DLSS: available (NGX initialized)");
#else
  REXGPU_INFO("DLSS: not built in (configure the SDK with REXGLUE_DLSS_SDK), only SMAA is "
              "available");
#endif
  return true;
}

bool DlssPass::InitializeGi() {
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 2;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 13;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (u32 i = 0; i < 2; ++i) {
    parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = UINT(std::size(parameters));
  root_desc.pParameters = parameters;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&gi_root_signature_)))) {
    return false;
  }
  gi_pipeline_.Attach(objects::CreateComputePipeline(device_, kSsgiCS, sizeof(kSsgiCS),
                                                     gi_root_signature_.Get()));
  return gi_pipeline_ != nullptr;
}

bool DlssPass::InitializeSmaa() {
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 8;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (u32 i = 0; i < 2; ++i) {
    parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
  for (u32 i = 0; i < 2; ++i) {
    samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = UINT(std::size(parameters));
  root_desc.pParameters = parameters;
  root_desc.NumStaticSamplers = UINT(std::size(samplers));
  root_desc.pStaticSamplers = samplers;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&smaa_root_signature_)))) {
    return false;
  }
  const std::pair<const void*, size_t> code[3] = {{kSmaaEdgesCS, sizeof(kSmaaEdgesCS)},
                                                  {kSmaaWeightsCS, sizeof(kSmaaWeightsCS)},
                                                  {kSmaaBlendCS, sizeof(kSmaaBlendCS)}};
  for (u32 i = 0; i < 3; ++i) {
    smaa_pipelines_[i].Attach(objects::CreateComputePipeline(
        device_, code[i].first, code[i].second, smaa_root_signature_.Get()));
    if (!smaa_pipelines_[i]) {
      return false;
    }
  }
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE DlssPass::CpuDescriptor(u32 index) const {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * descriptor_size_;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DlssPass::GpuDescriptor(u32 index) const {
  D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += UINT64(index) * descriptor_size_;
  return handle;
}

void DlssPass::AgeRetiredFeatures(bool release_all) {
  for (size_t i = 0; i < retired_.size();) {
    RetiredFeature& retired = retired_[i];
    if (release_all || retired.frames_left == 0) {
#if REX_DLSS
      if (retired.dlss) {
        NVSDK_NGX_D3D12_ReleaseFeature(retired.dlss);
      }
#endif
      if (retired.nr && nr_release_) {
        nr_release_(retired.nr);
      }
      if (retired.fsr && ffx_destroy_) {
        ffx_destroy_(&retired.fsr, nullptr);
      }
      retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      --retired.frames_left;
      ++i;
    }
  }
}

void DlssPass::Shutdown() {
  // Only called once the GPU is idle (context shutdown), so everything can go now.
  if (feature_) {
    retired_.push_back({feature_, nullptr, 0});
    feature_ = nullptr;
  }
  if (nr_feature_) {
    retired_.push_back({nullptr, nr_feature_, 0});
    nr_feature_ = nullptr;
  }
  if (fsr_context_) {
    retired_.push_back({nullptr, nullptr, 0, fsr_context_});
    fsr_context_ = nullptr;
  }
  fsr_key_ = {};
  AgeRetiredFeatures(true);
  feature_key_ = {};
  nr_ready_ = false;
  nr_initialize_attempted_ = false;
#if REX_DLSS
  if (parameters_) {
    NVSDK_NGX_D3D12_DestroyParameters(parameters_);
    parameters_ = nullptr;
  }
  if (ngx_initialized_ && device_) {
    NVSDK_NGX_D3D12_Shutdown1(device_);
  }
#endif
  ngx_initialized_ = false;
  available_ = false;
  color_out_.Reset();
  depth_out_.Reset();
  motion_out_.Reset();
  nr_out_.Reset();
  stats_.Reset();
  stats_zero_.Reset();
  sharpen_out_.Reset();
  gi_out_.Reset();
  gi_pipeline_.Reset();
  gi_root_signature_.Reset();
  for (u32 i = 0; i < kFrameGenerationSlots; ++i) {
    fg_depth_[i].Reset();
    fg_motion_[i].Reset();
  }
  fg_textures_readable_ = false;
  cas_pipeline_.Reset();
  smaa_color_.Reset();
  smaa_edges_.Reset();
  smaa_weights_.Reset();
  smaa_area_.Reset();
  smaa_search_.Reset();
  smaa_upload_.Reset();
  smaa_tables_uploaded_ = false;
  for (auto& pipeline : smaa_pipelines_) {
    pipeline.Reset();
  }
  smaa_root_signature_.Reset();
  dlss_supported_ = false;
  stats_readback_.Reset();
  textures_width_ = textures_height_ = 0;
  heap_.Reset();
  inputs_pipeline_.Reset();
  compose_pipeline_.Reset();
  root_signature_.Reset();
}

// The NGX parameter block is the driver's own object. Unsigned values go through vtable slot 3 and
// resources through the 64-bit setter (slot 0); floats sit at a slot that differs from the public
// header, found once by a round trip (InitializeNeuralRendering).
void DlssPass::SetParameterU32(const char* name, u32 value) {
  void** table = *reinterpret_cast<void***>(parameters_);
  reinterpret_cast<void (*)(void*, const char*, unsigned int)>(table[3])(parameters_, name, value);
}

void DlssPass::SetParameterF32(const char* name, f32 value) {
  if (float_slot_ < 0) {
    return;
  }
  void** table = *reinterpret_cast<void***>(parameters_);
  reinterpret_cast<void (*)(void*, const char*, float)>(table[float_slot_])(parameters_, name,
                                                                           value);
}

void DlssPass::SetParameterResource(const char* name, ID3D12Resource* resource) {
  void** table = *reinterpret_cast<void***>(parameters_);
  reinterpret_cast<void (*)(void*, const char*, unsigned long long)>(table[0])(
      parameters_, name, reinterpret_cast<unsigned long long>(resource));
}

bool DlssPass::InitializeNeuralRendering() {
  if (nr_initialize_attempted_) {
    return nr_ready_;
  }
  nr_initialize_attempted_ = true;
  if (!available_ || !dlss_supported_ || !parameters_) {
    return false;
  }
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring directory(exe_path);
  directory.resize(directory.find_last_of(L"\\/") + 1);
  HMODULE forwarder = LoadLibraryW((directory + L"nvngx.dll_nr_forwarder.dll").c_str());
  if (!forwarder) {
    REXGPU_WARN("DLSS NR: nvngx.dll_nr_forwarder.dll is missing next to the executable");
    return false;
  }
  nr_forwarder_ = forwarder;
  auto load = reinterpret_cast<int (*)(const wchar_t*)>(GetProcAddress(forwarder, "rex_nr_load"));
  auto init = reinterpret_cast<NrInit>(GetProcAddress(forwarder, "rex_nr_init"));
  nr_create_ = reinterpret_cast<NrCreate>(GetProcAddress(forwarder, "rex_nr_create"));
  nr_evaluate_ = reinterpret_cast<NrEvaluate>(GetProcAddress(forwarder, "rex_nr_evaluate"));
  nr_release_ = reinterpret_cast<NrRelease>(GetProcAddress(forwarder, "rex_nr_release"));
  if (!load || !init || !nr_create_ || !nr_evaluate_ || !nr_release_) {
    REXGPU_WARN("DLSS NR: the forwarder lacks its entry points");
    return false;
  }
  if (!load((directory + L"nvngx_dlssnr.dll").c_str())) {
    REXGPU_WARN("DLSS NR: nvngx_dlssnr.dll (NVIDIA's Neural Rendering model) is missing next to "
                "the executable, or is not a model this build understands");
    return false;
  }
  // 0x15 is the NGX API version of the 310 SDK; the id is this runtime's own.
  const int init_result = init(0x52455847ull, L".", device_, 0x15, parameters_);
  if (init_result != 1) {
    REXGPU_WARN("DLSS NR: the model did not initialize (0x{:08X})", static_cast<u32>(init_result));
    return false;
  }

  // Find the float setter: write through each candidate, read back through the getter 8 slots on.
  void** table = *reinterpret_cast<void***>(parameters_);
  constexpr const char* kProbe = "DLSSNR.RexFloatProbe";
  for (int slot : {1, 6, 2, 5, 7, 4}) {
    reinterpret_cast<void (*)(void*, const char*, float)>(table[slot])(parameters_, kProbe, 0.375f);
    float back = 0.0f;
    const int got =
        reinterpret_cast<int (*)(void*, const char*, float*)>(table[slot + 8])(parameters_, kProbe,
                                                                             &back);
    if (got == 1 && back == 0.375f) {
      float_slot_ = slot;
      break;
    }
  }
  if (float_slot_ < 0) {
    REXGPU_WARN("DLSS NR: float parameters cannot be set; intensity and strengths keep the "
                "model's defaults");
  }
  nr_ready_ = true;
  REXGPU_INFO("DLSS NR: model loaded (float parameters at slot {})", float_slot_);
  return true;
}

bool DlssPass::EnsureNrFeature(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                               u32 width, u32 height) {
  const bool same_size = nr_feature_width_ == width && nr_feature_height_ == height;
  if (nr_feature_ && same_size && nr_feature_settings_ == inputs.nr) {
    nr_pending_frames_ = 0;
    return true;
  }
  if (nr_feature_ && same_size) {
    // The tuning changed: rebuild once it has stayed the same for a moment. Rebuilding on every
    // change exhausts the driver and the model stops answering until the process restarts.
    if (!(nr_pending_settings_ == inputs.nr)) {
      nr_pending_settings_ = inputs.nr;
      nr_pending_frames_ = 0;
      return true;
    }
    if (++nr_pending_frames_ < 30) {
      return true;
    }
  }
  if (nr_feature_) {
    retired_.push_back({nullptr, nr_feature_, kRetireFrames});
    nr_feature_ = nullptr;
  }
  SetParameterU32("DLSSNR.Enabled", 1);
  SetParameterU32("DLSSNR.Width", width);
  SetParameterU32("DLSSNR.Height", height);
  SetParameterU32("CreationNodeMask", 1);
  SetParameterU32("VisibilityNodeMask", 1);
  SetParameterU32("DLSSNR.Hint.Render.Preset", 0);
  SetParameterU32("DLSSNR.Style", inputs.nr.style);
  SetParameterF32("DLSSNR.Intensity", inputs.nr.intensity);
  SetParameterF32("DLSSNR.LocalStructureStrength", inputs.nr.local_structure);
  SetParameterF32("DLSSNR.LocalToneStrength", inputs.nr.local_tone);
  SetParameterF32("DLSSNR.SkinStructureStrength", inputs.nr.skin_structure);
  void* handle = nullptr;
  const int result = nr_create_(command_list, 18, parameters_, &handle);
  if (result != 1 || !handle) {
    REXGPU_WARN("DLSS NR: cannot create the model's feature at {}x{} (0x{:08X})", width, height,
                static_cast<u32>(result));
    nr_failed_ = true;
    return false;
  }
  nr_feature_ = handle;
  nr_feature_settings_ = inputs.nr;
  nr_pending_settings_ = inputs.nr;
  nr_pending_frames_ = 0;
  nr_feature_width_ = width;
  nr_feature_height_ = height;
  REXGPU_INFO("DLSS NR: feature created at {}x{}, style {}, intensity {:.2f}", width, height,
              inputs.nr.style, inputs.nr.intensity);
  return true;
}

bool DlssPass::EvaluateNr(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                          ID3D12Resource* color, u32 color_x, u32 color_y, u32 width, u32 height) {
  SetParameterResource("DLSSNR.Color", color);
  SetParameterResource("DLSSNR.Depth", depth_out_.Get());
  SetParameterResource("DLSSNR.MVec", motion_out_.Get());
  SetParameterResource("DLSSNR.Output", nr_out_.Get());
  // The block is shared with DLSS, so everything the model reads is written again every time.
  SetParameterU32("DLSSNR.Enabled", 1);
  SetParameterU32("DLSSNR.Width", width);
  SetParameterU32("DLSSNR.Height", height);
  SetParameterU32("DLSSNR.DepthInverted", 0);
  SetParameterU32("DLSSNR.Reset", inputs.reset ? 1 : 0);
  const struct {
    const char* prefix;
    u32 x, y, w, h;
  } subrects[] = {{"DLSSNR.Color", color_x, color_y, width, height},
                  {"DLSSNR.Output", 0, 0, width, height},
                  // Depth and motion stay at the rendered size.
                  {"DLSSNR.Depth", 0, 0, inputs.render_width, inputs.render_height},
                  {"DLSSNR.MVec", 0, 0, inputs.render_width, inputs.render_height}};
  for (const auto& subrect : subrects) {
    const std::string prefix(subrect.prefix);
    SetParameterU32((prefix + "SubrectBaseX").c_str(), subrect.x);
    SetParameterU32((prefix + "SubrectBaseY").c_str(), subrect.y);
    SetParameterU32((prefix + "SubrectWidth").c_str(), subrect.w);
    SetParameterU32((prefix + "SubrectHeight").c_str(), subrect.h);
  }
  // Motion vectors are in rendered pixels; the model wants pixels of the frame it is given.
  SetParameterF32("DLSSNR.MVecScaleX", f32(width) / f32(inputs.render_width));
  SetParameterF32("DLSSNR.MVecScaleY", f32(height) / f32(inputs.render_height));
  SetParameterU32("DLSSNR.Style", nr_feature_settings_.style);
  SetParameterF32("DLSSNR.Intensity", nr_feature_settings_.intensity);
  SetParameterF32("DLSSNR.LocalStructureStrength", nr_feature_settings_.local_structure);
  SetParameterF32("DLSSNR.LocalToneStrength", nr_feature_settings_.local_tone);
  SetParameterF32("DLSSNR.SkinStructureStrength", nr_feature_settings_.skin_structure);
  const int result = nr_evaluate_(command_list, nr_feature_, parameters_);
  if (result != 1) {
    REXGPU_WARN("DLSS NR: evaluation failed (0x{:08X}), Neural Rendering is off",
                static_cast<u32>(result));
    nr_failed_ = true;
    return false;
  }
  return true;
}

void DlssPass::Sharpen(ID3D12GraphicsCommandList* command_list, f32 sharpness, u32 source_slot,
                       u32 source_x, u32 source_y, u32 width, u32 height) {
  // CasSetup() for sharpening without scaling: the scaling terms are identity and only the
  // sharpness term is read.
  const f32 sharp = -1.0f / (8.0f + (5.0f - 8.0f) * std::clamp(sharpness, 0.0f, 1.0f));
  const f32 scale = 1.0f;
  const f32 offset = 0.0f;
  const f32 eight = 8.0f;
  struct {
    u32 const0[4];
    u32 const1[4];
    u32 origin[2];
    u32 size[2];
  } constants = {{std::bit_cast<u32>(scale), std::bit_cast<u32>(scale), std::bit_cast<u32>(offset),
                  std::bit_cast<u32>(offset)},
                 {std::bit_cast<u32>(sharp), 0, std::bit_cast<u32>(eight), 0},
                 {source_x, source_y},
                 {width, height}};
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(cas_pipeline_.Get());
  command_list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(u32), &constants, 0);
  command_list->SetComputeRootDescriptorTable(1, GpuDescriptor(source_slot));
  command_list->SetComputeRootDescriptorTable(2, GpuDescriptor(kSlotSharpenUAV));
  command_list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
}

void DlssPass::Compose(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                       u32 original_slot, u32 original_x, u32 original_y, u32 width, u32 height) {
  struct {
    u32 origin[2];
    u32 size[2];
    f32 detail_strength;
    f32 colour_strength;
    f32 max_ratio;
    u32 debug_view;
  } constants = {{original_x, original_y},
                 {width, height},
                 inputs.nr_detail_strength,
                 inputs.nr_colour_strength,
                 4.0f,
                 inputs.nr_debug_view};
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(compose_pipeline_.Get());
  command_list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(u32), &constants, 0);
  command_list->SetComputeRootDescriptorTable(1, GpuDescriptor(original_slot));
  command_list->SetComputeRootDescriptorTable(2, GpuDescriptor(kSlotNrOutUAV));
  command_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

bool DlssPass::EnsureStatsBuffers() {
  if (stats_) {
    return true;
  }
  auto create = [&](const D3D12_HEAP_PROPERTIES& heap, D3D12_RESOURCE_FLAGS flags,
                    D3D12_RESOURCE_STATES state, u64 size,
                    Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                      nullptr, IID_PPV_ARGS(&out)));
  };
  if (!create(objects::kHeapPropertiesDefault, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 8, stats_) ||
      !create(objects::kHeapPropertiesUpload, D3D12_RESOURCE_FLAG_NONE,
              D3D12_RESOURCE_STATE_GENERIC_READ, 8, stats_zero_) ||
      !create(objects::kHeapPropertiesReadback, D3D12_RESOURCE_FLAG_NONE,
              D3D12_RESOURCE_STATE_COPY_DEST, 8 * kStatsSlots, stats_readback_)) {
    stats_.Reset();
    return false;
  }
  void* zero = nullptr;
  if (SUCCEEDED(stats_zero_->Map(0, nullptr, &zero))) {
    std::memset(zero, 0, 8);
    stats_zero_->Unmap(0, nullptr);
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = 2;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(stats_.Get(), nullptr, &uav, CpuDescriptor(kSlotStatsUAV));
  return true;
}

void DlssPass::RecordStats(ID3D12GraphicsCommandList* command_list, u32 pixels) {
  // Read a slot written kStatsSlots - 1 passes ago (long done on the GPU), every 2 seconds or so.
  const u32 slot = static_cast<u32>(stats_frame_ % kStatsSlots);
  if (stats_frame_ >= kStatsSlots && stats_frame_ % 120 == 0) {
    const u32 old_slot = static_cast<u32>((stats_frame_ + 1) % kStatsSlots);
    const D3D12_RANGE range = {old_slot * 8, old_slot * 8 + 8};
    void* mapped = nullptr;
    if (stats_pixels_[old_slot] && SUCCEEDED(stats_readback_->Map(0, &range, &mapped))) {
      const u32* values = reinterpret_cast<const u32*>(static_cast<const u8*>(mapped) + old_slot * 8);
      const f64 total = stats_pixels_[old_slot];
      REXGPU_INFO("DLSS NR: the model changed {:.2f}% of pixels by more than 2/255, mean change "
                  "{:.3f}/255",
                  100.0 * values[1] / total, values[0] / total);
      const D3D12_RANGE none = {0, 0};
      stats_readback_->Unmap(0, &none);
    }
  }
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = stats_.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_list->ResourceBarrier(1, &barrier);
  command_list->CopyBufferRegion(stats_readback_.Get(), slot * 8, stats_.Get(), 0, 8);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  command_list->ResourceBarrier(1, &barrier);
  // Zeroed for the next pass.
  command_list->CopyBufferRegion(stats_.Get(), 0, stats_zero_.Get(), 0, 8);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  command_list->ResourceBarrier(1, &barrier);
  stats_pixels_[slot] = pixels;
  ++stats_frame_;
}

void DlssPass::UploadSmaaTables(ID3D12GraphicsCommandList* command_list) {
  // SMAA's precomputed area (RG8, 160x560) and search (R8, 64x16) lookup textures.
  struct Table {
    const unsigned char* bytes;
    u32 width, height, pitch;
    DXGI_FORMAT format;
    Microsoft::WRL::ComPtr<ID3D12Resource>* texture;
    u32 slot;
  } tables[2] = {
      {areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM,
       &smaa_area_, kSlotSmaaAreaSRV},
      {searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM,
       &smaa_search_, kSlotSmaaSearchSRV},
  };
  auto aligned_pitch = [](u32 pitch) {
    return (pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  };
  const u64 area_size = u64(aligned_pitch(AREATEX_PITCH)) * AREATEX_HEIGHT;
  const u64 search_offset =
      (area_size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
      ~u64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  const u64 upload_size = search_offset + u64(aligned_pitch(SEARCHTEX_PITCH)) * SEARCHTEX_HEIGHT;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = upload_size;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&objects::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
                                              &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&smaa_upload_)))) {
    return;
  }
  u8* mapped = nullptr;
  if (FAILED(smaa_upload_->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    return;
  }
  const u64 offsets[2] = {0, search_offset};
  for (u32 t = 0; t < 2; ++t) {
    const Table& table = tables[t];
    for (u32 row = 0; row < table.height; ++row) {
      std::memcpy(mapped + offsets[t] + u64(row) * aligned_pitch(table.pitch),
                  table.bytes + u64(row) * table.pitch, table.pitch);
    }
  }
  smaa_upload_->Unmap(0, nullptr);
  for (u32 t = 0; t < 2; ++t) {
    const Table& table = tables[t];
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = table.width;
    desc.Height = table.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = table.format;
    desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateCommittedResource(&objects::kHeapPropertiesDefault,
                                                D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&(*table.texture))))) {
      return;
    }
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = smaa_upload_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = offsets[t];
    source.PlacedFootprint.Footprint = {table.format, table.width, table.height, 1,
                                        aligned_pitch(table.pitch)};
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = table.texture->Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command_list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = table.texture->Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kShaderRead;
    command_list->ResourceBarrier(1, &barrier);
    device_->CreateShaderResourceView(table.texture->Get(), nullptr, CpuDescriptor(table.slot));
  }
  smaa_tables_uploaded_ = true;
}

void DlssPass::RunSmaa(ID3D12GraphicsCommandList* command_list, ID3D12Resource* source,
                       D3D12_RESOURCE_STATES source_state, u32 x, u32 y, u32 width,
                       u32 height) {
  Barriers barriers;
  // SMAA reads its colour through normalised coordinates, so it gets a copy of exactly the scene.
  barriers.Add(source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barriers.Add(smaa_color_.Get(),
               smaa_color_needs_transition_ ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : kShaderRead,
               D3D12_RESOURCE_STATE_COPY_DEST);
  barriers.Submit(command_list);
  smaa_color_needs_transition_ = false;
  D3D12_TEXTURE_COPY_LOCATION copy_source = {};
  copy_source.pResource = source;
  copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION copy_dest = {};
  copy_dest.pResource = smaa_color_.Get();
  copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_BOX box = {x, y, 0, x + width, y + height, 1};
  command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, &box);
  barriers.Add(source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
  barriers.Add(smaa_color_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
  barriers.Submit(command_list);

  const struct {
    f32 metrics[4];
    u32 size[2];
    u32 padding[2];
  } constants = {{1.0f / f32(width), 1.0f / f32(height), f32(width), f32(height)},
                 {width, height},
                 {0, 0}};
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(smaa_root_signature_.Get());
  command_list->SetComputeRoot32BitConstants(0, 8, &constants, 0);
  const u32 groups_x = (width + 7) / 8;
  const u32 groups_y = (height + 7) / 8;
  const u32 srv_tables[3] = {kSlotSmaaColorSRV, kSlotSmaaEdgesSRV, kSlotSmaaColorSRV};
  const u32 uav_tables[3] = {kSlotSmaaEdgesUAV, kSlotSmaaWeightsUAV, kSlotColorOutUAV};
  ID3D12Resource* outputs[2] = {smaa_edges_.Get(), smaa_weights_.Get()};
  for (u32 pass = 0; pass < 3; ++pass) {
    command_list->SetPipelineState(smaa_pipelines_[pass].Get());
    command_list->SetComputeRootDescriptorTable(1, GpuDescriptor(srv_tables[pass]));
    command_list->SetComputeRootDescriptorTable(2, GpuDescriptor(uav_tables[pass]));
    command_list->Dispatch(groups_x, groups_y, 1);
    if (pass < 2) {
      barriers.Add(outputs[pass], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
      barriers.Submit(command_list);
    }
  }
  barriers.Add(smaa_edges_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barriers.Add(smaa_weights_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barriers.Add(color_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
  barriers.Submit(command_list);
}

bool DlssPass::EnsureTextures(const Inputs& inputs, DXGI_FORMAT color_format) {
  if (textures_width_ == inputs.width && textures_height_ == inputs.height &&
      textures_color_format_ == color_format && color_out_) {
    return true;
  }
  // Sized for the whole scene, so the quality mode can change without new textures. This only
  // happens on a resolution change (a restart), so nothing in flight uses the old ones.
  color_out_.Reset();
  depth_out_.Reset();
  motion_out_.Reset();
  nr_out_.Reset();
  auto create = [&](DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = inputs.width;
    desc.Height = inputs.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device_->CreateCommittedResource(
        &objects::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
  };
  if (!create(color_format, color_out_) || !create(color_format, nr_out_) ||
      !create(DXGI_FORMAT_R32_FLOAT, depth_out_) ||
      !create(DXGI_FORMAT_R16G16_FLOAT, motion_out_) ||
      !create(color_format, smaa_color_) || !create(DXGI_FORMAT_R8G8_UNORM, smaa_edges_) ||
      !create(color_format, sharpen_out_) || !create(color_format, gi_out_) ||
      [&] {
        for (u32 i = 0; i < kFrameGenerationSlots; ++i) {
          if (!create(DXGI_FORMAT_R32_FLOAT, fg_depth_[i]) ||
              !create(DXGI_FORMAT_R16G16_FLOAT, fg_motion_[i])) {
            return false;
          }
        }
        return true;
      }() == false ||
      !create(DXGI_FORMAT_R8G8B8A8_UNORM, smaa_weights_)) {
    REXGPU_WARN("DLSS: cannot create {}x{} working textures", inputs.width, inputs.height);
    return false;
  }
  device_->CreateUnorderedAccessView(depth_out_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotDepthOutUAV));
  device_->CreateUnorderedAccessView(motion_out_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotMotionOutUAV));
  device_->CreateShaderResourceView(color_out_.Get(), nullptr, CpuDescriptor(kSlotColorOutSRV));
  device_->CreateUnorderedAccessView(nr_out_.Get(), nullptr, nullptr, CpuDescriptor(kSlotNrOutUAV));
  device_->CreateUnorderedAccessView(color_out_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotColorOutUAV));
  device_->CreateShaderResourceView(nr_out_.Get(), nullptr, CpuDescriptor(kSlotNrOutSRV));
  device_->CreateUnorderedAccessView(gi_out_.Get(), nullptr, nullptr, CpuDescriptor(kSlotGiOutUAV));
  device_->CreateShaderResourceView(gi_out_.Get(), nullptr, CpuDescriptor(kSlotGiOutSRV));
  device_->CreateUnorderedAccessView(sharpen_out_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotSharpenUAV));
  device_->CreateUnorderedAccessView(sharpen_out_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotSharpenUAVSpare));
  device_->CreateShaderResourceView(smaa_color_.Get(), nullptr, CpuDescriptor(kSlotSmaaColorSRV));
  device_->CreateShaderResourceView(smaa_weights_.Get(), nullptr,
                                    CpuDescriptor(kSlotSmaaWeightsSRV));
  device_->CreateShaderResourceView(smaa_edges_.Get(), nullptr, CpuDescriptor(kSlotSmaaEdgesSRV));
  device_->CreateUnorderedAccessView(smaa_edges_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotSmaaEdgesUAV));
  device_->CreateUnorderedAccessView(smaa_weights_.Get(), nullptr, nullptr,
                                     CpuDescriptor(kSlotSmaaWeightsUAV));
  // SMAA's colour copy rests readable; the others rest writable like every working texture.
  smaa_color_needs_transition_ = true;
  fg_textures_readable_ = false;
  textures_width_ = inputs.width;
  textures_height_ = inputs.height;
  textures_color_format_ = color_format;
  return true;
}

bool DlssPass::IsFsrSupported() {
#if REX_FFX
  if (ffx_attempted_) {
    return ffx_ready_;
  }
  ffx_attempted_ = true;
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring directory(exe_path);
  directory.resize(directory.find_last_of(L"\\/") + 1);
  HMODULE loader = LoadLibraryW((directory + L"amd_fidelityfx_loader_dx12.dll").c_str());
  if (!loader) {
    REXGPU_WARN("FSR: amd_fidelityfx_loader_dx12.dll is missing next to the executable");
    return false;
  }
  ffx_loader_ = loader;
  ffx_create_ = reinterpret_cast<FfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
  ffx_destroy_ = reinterpret_cast<FfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
  ffx_dispatch_ = reinterpret_cast<FfxDispatch>(GetProcAddress(loader, "ffxDispatch"));
  ffx_ready_ = ffx_create_ && ffx_destroy_ && ffx_dispatch_;
  if (ffx_ready_) {
    REXGPU_INFO("FSR: AMD FidelityFX loaded");
  } else {
    REXGPU_WARN("FSR: the FidelityFX loader lacks its entry points");
  }
  return ffx_ready_;
#else
  return false;
#endif
}

bool DlssPass::EnsureFsrContext(const Inputs& inputs) {
#if REX_FFX
  // The context is sized for the largest rendered and output sizes; the quality mode only
  // changes the render size passed per dispatch, so it does not need a new context.
  const DlssKey key = {inputs.width, inputs.height, inputs.width, inputs.height, 0, 0};
  if (fsr_context_ && fsr_key_ == key) {
    return true;
  }
  if (fsr_context_) {
    retired_.push_back({nullptr, nullptr, kRetireFrames, fsr_context_});
    fsr_context_ = nullptr;
  }
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device_;
  ffxCreateContextDescUpscale create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  create.header.pNext = &backend.header;
  // The scene is 8-bit and sRGB encoded, and the title has no exposure the pass could pass on.
  create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  create.maxRenderSize = {inputs.width, inputs.height};
  create.maxUpscaleSize = {inputs.width, inputs.height};
  void* context = nullptr;
  const int32_t result = ffx_create_(&context, &create.header, nullptr);
  if (result != FFX_API_RETURN_OK || !context) {
    REXGPU_WARN("FSR: cannot create the upscaler at {}x{} ({})", inputs.width, inputs.height,
                result);
    return false;
  }
  fsr_context_ = context;
  fsr_key_ = key;
  REXGPU_INFO("FSR: upscaler created for {}x{}", inputs.width, inputs.height);
  return true;
#else
  (void)inputs;
  return false;
#endif
}

bool DlssPass::DispatchFsr(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                           ID3D12Resource* color) {
#if REX_FFX
  ffxDispatchDescUpscale dispatch = {};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = command_list;
  dispatch.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.depth = ffxApiGetResourceDX12(depth_out_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.motionVectors =
      ffxApiGetResourceDX12(motion_out_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.output = ffxApiGetResourceDX12(color_out_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch.jitterOffset = {inputs.jitter_x, inputs.jitter_y};
  dispatch.motionVectorScale = {1.0f, 1.0f};  // already in rendered pixels
  dispatch.renderSize = {inputs.render_width, inputs.render_height};
  dispatch.upscaleSize = {inputs.width, inputs.height};
  dispatch.frameTimeDelta = inputs.frame_time_ms;
  dispatch.preExposure = 1.0f;
  dispatch.reset = inputs.reset;
  dispatch.cameraNear = inputs.camera_near;
  dispatch.cameraFar = inputs.camera_far;
  dispatch.cameraFovAngleVertical = inputs.camera_fov_y;
  dispatch.viewSpaceToMetersFactor = 1.0f;
  const int32_t result = ffx_dispatch_(&fsr_context_, &dispatch.header);
  if (result != FFX_API_RETURN_OK) {
    REXGPU_WARN("FSR: dispatch failed ({}), the upscaler is off", result);
    return false;
  }
  return true;
#else
  (void)command_list;
  (void)inputs;
  (void)color;
  return false;
#endif
}

bool DlssPass::EnsureDlssFeature(ID3D12GraphicsCommandList* command_list, const Inputs& inputs) {
#if REX_DLSS
  const DlssKey key = {inputs.width,        inputs.height,   inputs.render_width,
                       inputs.render_height, inputs.quality, inputs.preset};
  if (feature_ && feature_key_ == key) {
    return true;
  }
  if (feature_) {
    retired_.push_back({feature_, nullptr, kRetireFrames});
    feature_ = nullptr;
  }
  // The preset hint is per quality mode; set them all so the chosen model applies to any mode.
  for (const char* hint :
       {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
    NVSDK_NGX_Parameter_SetUI(parameters_, hint, inputs.preset);
  }
  NVSDK_NGX_DLSS_Create_Params create_params = {};
  create_params.Feature.InWidth = inputs.render_width;
  create_params.Feature.InHeight = inputs.render_height;
  create_params.Feature.InTargetWidth = inputs.width;
  create_params.Feature.InTargetHeight = inputs.height;
  create_params.Feature.InPerfQualityValue =
      static_cast<NVSDK_NGX_PerfQuality_Value>(inputs.quality);
  create_params.InFeatureCreateFlags =
      NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Result result =
      NGX_D3D12_CREATE_DLSS_EXT(command_list, 1, 1, &feature_, parameters_, &create_params);
  if (NVSDK_NGX_FAILED(result) || !feature_) {
    REXGPU_WARN("DLSS: cannot create the feature {}x{} -> {}x{} (0x{:08X})", inputs.render_width,
                inputs.render_height, inputs.width, inputs.height, static_cast<u32>(result));
    feature_ = nullptr;
    return false;
  }
  feature_key_ = key;
  REXGPU_INFO("DLSS: feature created {}x{} -> {}x{}, quality {}, preset {}", inputs.render_width,
              inputs.render_height, inputs.width, inputs.height, inputs.quality, inputs.preset);
  return true;
#else
  (void)command_list;
  (void)inputs;
  return false;
#endif
}

void DlssPass::Record(ID3D12GraphicsCommandList* command_list, const Inputs& inputs) {
#if REX_DLSS
  AgeRetiredFeatures(false);
  const bool nr = inputs.nr.enabled && InitializeNeuralRendering() && !nr_failed_;
  const bool sharpen = inputs.sharpen > 0.0f;
  if (!available_ || failed_ ||
      (!inputs.dlss && !inputs.smaa && !nr && !sharpen && !inputs.frame_generation &&
       !inputs.gi) ||
      !inputs.color ||
      !inputs.depth ||
      !inputs.width || !inputs.height || !inputs.render_width || !inputs.render_height) {
    return;
  }
  const D3D12_RESOURCE_DESC color_desc = inputs.color->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = inputs.depth->GetDesc();
  const DXGI_FORMAT color_format = TypedColorFormat(color_desc.Format);
  const DXGI_FORMAT depth_srv_format = DepthSRVFormat(depth_desc.Format);
  if (depth_srv_format == DXGI_FORMAT_UNKNOWN || color_desc.SampleDesc.Count != 1 ||
      depth_desc.SampleDesc.Count != 1) {
    REXGPU_WARN("DLSS: unsupported scene targets (colour format {}, depth format {}, {}x/{}x "
                "MSAA), DLSS is off",
                static_cast<u32>(color_desc.Format), static_cast<u32>(depth_desc.Format),
                color_desc.SampleDesc.Count, depth_desc.SampleDesc.Count);
    failed_ = true;
    return;
  }
  if (!EnsureTextures(inputs, color_format)) {
    failed_ = true;
    return;
  }
  const bool smaa = inputs.smaa;
  if (smaa && !smaa_tables_uploaded_) {
    UploadSmaaTables(command_list);
    if (!smaa_tables_uploaded_) {
      REXGPU_WARN("SMAA: cannot create its lookup textures, the pass is off");
      failed_ = true;
      return;
    }
  }
  const bool fsr = !smaa && inputs.dlss && inputs.fsr && IsFsrSupported() &&
                   EnsureFsrContext(inputs);
  const bool dlss = !smaa && inputs.dlss && !inputs.fsr && dlss_supported_ &&
                    EnsureDlssFeature(command_list, inputs);
  if (!smaa && inputs.dlss && !fsr && !dlss) {
    // The chosen upscaler is not available here; the caller stops shrinking the scene.
    failed_ = true;
    return;
  }
  // Neural Rendering runs on the rendered scene (before anti-aliasing) or on its output.
  const bool nr_before = nr && (inputs.nr.before_dlss || (!dlss && !fsr && !smaa));
  const u32 nr_width = nr_before ? inputs.render_width : inputs.width;
  const u32 nr_height = nr_before ? inputs.render_height : inputs.height;
  const bool run_nr = nr && EnsureNrFeature(command_list, inputs, nr_width, nr_height);

  // This frame's views of the scene targets.
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = depth_srv_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(inputs.depth, &srv, CpuDescriptor(kSlotDepthSRV));
  srv.Format = color_format;
  device_->CreateShaderResourceView(inputs.color, &srv, CpuDescriptor(kSlotSceneSRV));

  Barriers barriers;
  barriers.Add(inputs.depth, inputs.depth_state, kShaderRead);
  barriers.Add(inputs.color, inputs.color_state, kShaderRead);
  barriers.Submit(command_list);

  // Depth copy and motion vectors, over the rendered part.
  InputsConstants constants;
  std::copy(inputs.reproject.begin(), inputs.reproject.end(), constants.reproject);
  constants.depth_origin[0] = inputs.depth_x;
  constants.depth_origin[1] = inputs.depth_y;
  constants.size[0] = inputs.render_width;
  constants.size[1] = inputs.render_height;
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(inputs_pipeline_.Get());
  command_list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(u32), &constants, 0);
  command_list->SetComputeRootDescriptorTable(1, GpuDescriptor(kSlotDepthSRV));
  command_list->SetComputeRootDescriptorTable(2, GpuDescriptor(kSlotDepthOutUAV));
  command_list->Dispatch((inputs.render_width + 7) / 8, (inputs.render_height + 7) / 8, 1);

  barriers.Add(depth_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
  barriers.Add(motion_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
  barriers.Submit(command_list);

  if (inputs.frame_generation) {
    // A copy for frame generation, published to the presenter, which prepares it before Present.
    const u32 slot = fg_slot_;
    fg_slot_ = (fg_slot_ + 1) % kFrameGenerationSlots;
    const D3D12_RESOURCE_STATES fg_state =
        fg_textures_readable_ ? kShaderRead : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers.Add(depth_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(motion_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (u32 i = 0; i < kFrameGenerationSlots; ++i) {
      if (i == slot || !fg_textures_readable_) {
        barriers.Add(fg_depth_[i].Get(), fg_state,
                     i == slot ? D3D12_RESOURCE_STATE_COPY_DEST : kShaderRead);
        barriers.Add(fg_motion_[i].Get(), fg_state,
                     i == slot ? D3D12_RESOURCE_STATE_COPY_DEST : kShaderRead);
        barriers.Submit(command_list);
      }
    }
    fg_textures_readable_ = true;
    D3D12_BOX box = {0, 0, 0, inputs.render_width, inputs.render_height, 1};
    const std::pair<ID3D12Resource*, ID3D12Resource*> copies[2] = {
        {depth_out_.Get(), fg_depth_[slot].Get()}, {motion_out_.Get(), fg_motion_[slot].Get()}};
    for (const auto& [source, dest] : copies) {
      D3D12_TEXTURE_COPY_LOCATION from = {};
      from.pResource = source;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION to = {};
      to.pResource = dest;
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      command_list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    barriers.Add(depth_out_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kShaderRead);
    barriers.Add(motion_out_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kShaderRead);
    barriers.Add(fg_depth_[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
    barriers.Add(fg_motion_[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
    barriers.Submit(command_list);
    FrameGenerationFrame frame = inputs.frame_generation_camera;
    frame.depth = fg_depth_[slot];
    frame.motion = fg_motion_[slot];
    frame.render_width = inputs.render_width;
    frame.render_height = inputs.render_height;
    frame.jitter_x = inputs.jitter_x;
    frame.jitter_y = inputs.jitter_y;
    frame.frame_time_ms = inputs.frame_time_ms;
    frame.reset = inputs.reset;
    frame.camera_near = inputs.camera_near;
    frame.camera_far = inputs.camera_far;
    frame.camera_fov_y = inputs.camera_fov_y;
    inputs.frame_generation->Publish(frame);
  }

  // The chain. Each step reads the previous one's result; the scene render target is the first.
  ID3D12Resource* current = inputs.color;
  u32 current_slot = kSlotSceneSRV;
  u32 current_x = inputs.color_x;
  u32 current_y = inputs.color_y;
  bool nr_out_read = false;
  bool color_out_read = false;
  auto run_nr_step = [&](u32 width, u32 height) {
    if (!EvaluateNr(command_list, inputs, current, current_x, current_y, width, height)) {
      return;
    }
    // The model wrote nr_out_; compose it with the frame it was given, in place.
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nr_out_.Get();
    command_list->ResourceBarrier(1, &uav);
    if (!EnsureStatsBuffers()) {
      return;
    }
    Compose(command_list, inputs, current_slot, current_x, current_y, width, height);
    D3D12_RESOURCE_BARRIER stats_uav = {};
    stats_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    stats_uav.UAV.pResource = stats_.Get();
    command_list->ResourceBarrier(1, &stats_uav);
    RecordStats(command_list, width * height);
    barriers.Add(nr_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
    barriers.Submit(command_list);
    nr_out_read = true;
    current = nr_out_.Get();
    current_slot = kSlotNrOutSRV;
    current_x = current_y = 0;
  };
  // Global illumination first, on the rendered scene, so the upscaler accumulates its noise.
  bool gi_out_read = false;
  if (inputs.gi && !failed_) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = color_format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(current, &view, CpuDescriptor(kSlotGiColorSRV));
    view.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateShaderResourceView(depth_out_.Get(), &view, CpuDescriptor(kSlotGiDepthSRV));
    const f32 tan_y = std::tan(inputs.camera_fov_y * 0.5f);
    const f32 tan_x = tan_y * f32(inputs.render_width) / f32(inputs.render_height);
    const struct {
      u32 origin[2];
      u32 size[2];
      f32 near_z, far_z;
      f32 tan_half_fov[2];
      f32 radius, ao_strength, gi_strength;
      u32 frame;
    } gi_constants = {{current_x, current_y},
                   {inputs.render_width, inputs.render_height},
                   inputs.camera_near,
                   inputs.camera_far,
                   {tan_x, tan_y},
                   inputs.gi_radius,
                   inputs.gi_ao_strength,
                   inputs.gi_strength,
                   inputs.dlss ? gi_frame_++ : 0u};
    ID3D12DescriptorHeap* gi_heaps[] = {heap_.Get()};
    command_list->SetDescriptorHeaps(1, gi_heaps);
    command_list->SetComputeRootSignature(gi_root_signature_.Get());
    command_list->SetPipelineState(gi_pipeline_.Get());
    command_list->SetComputeRoot32BitConstants(0, sizeof(gi_constants) / sizeof(u32), &gi_constants, 0);
    command_list->SetComputeRootDescriptorTable(1, GpuDescriptor(kSlotGiColorSRV));
    command_list->SetComputeRootDescriptorTable(2, GpuDescriptor(kSlotGiOutUAV));
    command_list->Dispatch((inputs.render_width + 7) / 8, (inputs.render_height + 7) / 8, 1);
    barriers.Add(gi_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
    barriers.Submit(command_list);
    gi_out_read = true;
    current = gi_out_.Get();
    current_slot = kSlotGiOutSRV;
    current_x = current_y = 0;
  }
  if (run_nr && nr_before) {
    run_nr_step(inputs.render_width, inputs.render_height);
  }
  if (fsr) {
    // FSR reads its colour from the origin of the resource.
    if (current_x || current_y || !DispatchFsr(command_list, inputs, current)) {
      failed_ = true;
    } else {
      barriers.Add(color_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
      barriers.Submit(command_list);
      color_out_read = true;
      current = color_out_.Get();
      current_slot = kSlotColorOutSRV;
    }
  } else if (smaa) {
    RunSmaa(command_list, current, kShaderRead, current_x, current_y, inputs.width,
            inputs.height);
    color_out_read = true;
    current = color_out_.Get();
    current_slot = kSlotColorOutSRV;
    current_x = current_y = 0;
  } else if (dlss) {
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = current;
    eval.Feature.pInOutput = color_out_.Get();
    eval.pInDepth = depth_out_.Get();
    eval.pInMotionVectors = motion_out_.Get();
    eval.InJitterOffsetX = inputs.jitter_x;
    eval.InJitterOffsetY = inputs.jitter_y;
    eval.InRenderSubrectDimensions.Width = inputs.render_width;
    eval.InRenderSubrectDimensions.Height = inputs.render_height;
    eval.InReset = inputs.reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InColorSubrectBase.X = current_x;
    eval.InColorSubrectBase.Y = current_y;
    NVSDK_NGX_Result result =
        NGX_D3D12_EVALUATE_DLSS_EXT(command_list, feature_, parameters_, &eval);
    if (NVSDK_NGX_FAILED(result)) {
      REXGPU_WARN("DLSS: evaluation failed (0x{:08X}), DLSS is off", static_cast<u32>(result));
      failed_ = true;
    } else {
      barriers.Add(color_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
      barriers.Submit(command_list);
      color_out_read = true;
      current = color_out_.Get();
      current_slot = kSlotColorOutSRV;
      current_x = current_y = 0;
    }
  }
  if (run_nr && !nr_before && !failed_) {
    run_nr_step(inputs.width, inputs.height);
  }

  // Sharpening last, on whatever the chain produced (at the output size).
  bool sharpen_out_read = false;
  if (sharpen && !failed_) {
    Sharpen(command_list, inputs.sharpen, current_slot, current_x, current_y, inputs.width,
            inputs.height);
    barriers.Add(sharpen_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
    barriers.Submit(command_list);
    sharpen_out_read = true;
    current = sharpen_out_.Get();
    current_x = current_y = 0;
  }

  // Back into the scene render target, where the resolve picks it up. Super Resolution always
  // copies (its output is the full scene); DLAA or NR alone copy when a step ran.
  barriers.Add(depth_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barriers.Add(motion_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (current != inputs.color) {
    const bool full = current == color_out_.Get() || current == sharpen_out_.Get() || !nr_before;
    const u32 copy_width = full ? inputs.width : nr_width;
    const u32 copy_height = full ? inputs.height : nr_height;
    barriers.Add(current, kShaderRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(inputs.color, kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Submit(command_list);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = current;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = inputs.color;
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0, 0, 0, copy_width, copy_height, 1};
    command_list->CopyTextureRegion(&dest, inputs.color_x, inputs.color_y, 0, &source, &box);
    barriers.Add(current, D3D12_RESOURCE_STATE_COPY_SOURCE, kShaderRead);
    barriers.Add(inputs.color, D3D12_RESOURCE_STATE_COPY_DEST, inputs.color_state);
  } else {
    barriers.Add(inputs.color, kShaderRead, inputs.color_state);
  }
  if (nr_out_read) {
    barriers.Add(nr_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (color_out_read) {
    barriers.Add(color_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (sharpen_out_read) {
    barriers.Add(sharpen_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (gi_out_read) {
    barriers.Add(gi_out_.Get(), kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  barriers.Add(inputs.depth, kShaderRead, inputs.depth_state);
  barriers.Submit(command_list);
#else
  (void)command_list;
  (void)inputs;
#endif
}

}  // namespace rex::gpu::d3d12
