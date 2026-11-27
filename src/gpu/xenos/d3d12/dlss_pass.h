#pragma once

/**
 * @file        gpu/xenos/d3d12/dlss_pass.h
 * @brief       NVIDIA DLSS (DLAA or Super Resolution) and DLSS Neural Rendering applied in place to
 *              the guest's scene render target.
 *
 * The title's scene colour and depth stay in their host render targets. Before the scene is
 * resolved, the pass builds float depth and camera motion vectors and runs a chain of
 * [Neural Rendering] -> DLSS -> [Neural Rendering] whose result is copied back into the render
 * target, so the resolve and everything after it (post-processing, HUD) see it. For Super
 * Resolution the scene was drawn into the top-left render_width x render_height of the target and
 * DLSS upscales it to the whole target.
 *
 * DLSS is built with NGX only when REX_DLSS is defined (CMake option REXGLUE_DLSS_SDK); otherwise
 * the pass reports itself unavailable.
 */

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <vector>

#include <rex/types.h>

#include "gpu/xenos/host/d3d12/frame_generation_exchange.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace rex::gpu::d3d12 {

class DlssPass {
 public:
  struct Inputs {
    ID3D12Resource* color = nullptr;  // scene colour render target, R8G8B8A8
    D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_COMMON;
    u32 color_x = 0;
    u32 color_y = 0;
    // Linear colour is the stored colour times this (a resolve exponent bias, for a fixed-point
    // scene texture). Above 1 the chain runs on a linear float copy and DLSS in HDR mode.
    f32 color_scale = 1.0f;
    // CopySceneMotion wrote this frame's object motion (texture mode): it replaces the camera
    // motion where the scene draws wrote theirs.
    bool object_motion = false;
    // Shows the motion vectors instead of the scene (see dlss_motion_debug_cs).
    bool debug_motion = false;
    ID3D12Resource* depth = nullptr;  // scene depth render target
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
    u32 depth_x = 0;
    u32 depth_y = 0;
    u32 width = 0;  // scene (output) size in host pixels
    u32 height = 0;
    u32 render_width = 0;  // the part the scene was drawn into; equal to width for DLAA
    u32 render_height = 0;
    f32 jitter_x = 0.0f;  // sub-pixel offset the scene was drawn with, in pixels
    f32 jitter_y = 0.0f;
    std::array<f32, 16> reproject{};  // current clip -> previous clip, row vector, row-major
    bool reset = false;
    bool dlss = true;    // run DLSS (the scene was jittered for it)
    bool smaa = false;   // run SMAA instead (no jitter, any GPU)
    f32 sharpen = 0.0f;  // FidelityFX CAS at the end of the chain: 0 off, up to 1 strongest
    // Screen-space global illumination at the start of the chain (before the upscaler).
    bool gi = false;
    f32 gi_strength = 1.0f;     // indirect light
    f32 gi_ao_strength = 0.7f;  // ambient occlusion
    f32 gi_radius = 1.5f;       // in view units
    u32 quality = 5;   // NVSDK_NGX_PerfQuality_Value: 5 DLAA, 2 quality, 1 balanced, 0 performance
    u32 preset = 0;    // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 = default
    bool fsr = false;  // AMD FSR instead of DLSS as the upscaler (any GPU)
    // What FSR needs about the frame and the camera.
    f32 frame_time_ms = 16.7f;
    f32 camera_near = 0.1f;
    f32 camera_far = 1000.0f;
    f32 camera_fov_y = 1.0f;  // radians
    // Frame generation: publish this frame's depth, motion and camera to the presenter.
    FrameGenerationExchange* frame_generation = nullptr;
    FrameGenerationFrame frame_generation_camera;  // camera fields filled by the caller
    // DLSS Neural Rendering (NGX feature 18). Tuning is latched when the model's feature is
    // created; a change rebuilds it once the values have settled.
    struct NeuralRendering {
      bool enabled = false;
      bool before_dlss = false;  // on the rendered scene instead of DLSS's output
      u32 style = 0;             // 0 default, 1 natural, 2 cinematic
      f32 intensity = 1.0f;
      f32 local_structure = 1.0f;
      f32 local_tone = 1.0f;
      f32 skin_structure = -1.0f;  // -1 leaves the model's own value
      bool operator==(const NeuralRendering&) const = default;
    } nr;
    // How the model's answer is composed with the frame (not latched).
    f32 nr_detail_strength = 1.0f;
    f32 nr_colour_strength = 1.0f;
    u32 nr_debug_view = 0;  // 0 composed, 1 amplified difference, 2 the model's raw answer
  };

  DlssPass() = default;
  ~DlssPass();
  DlssPass(const DlssPass&) = delete;
  DlssPass& operator=(const DlssPass&) = delete;

  /** Loads NGX and checks DLSS support. Safe to call when DLSS is not built in. */
  bool Initialize(ID3D12Device* device);

  /** Where one depth resolve's object motion goes (the shader's constants, in order). */
  struct MotionCopy {
    u32 base_tiles;
    u32 pitch_tiles;
    u32 sample_count_log2[2];
    u32 tile_size[2];
    u32 source_origin[2];
    u32 size[2];
    u32 dest_origin[2];
    f32 ndc_to_pixels[2];
    u32 motion_region;
  };
  /**
   * Copies the object motion of a depth resolve's region out of the region after EDRAM (in the
   * UAV state, edram_size_bytes including that region) into the scene_width x scene_height scene
   * motion texture, and clears it in EDRAM.
   */
  void CopySceneMotion(ID3D12GraphicsCommandList* command_list, ID3D12Resource* edram,
                       u32 edram_size_bytes, const MotionCopy& copy, u32 scene_width,
                       u32 scene_height);
  void Shutdown();
  // False once a pass failed, so the scene stops being jittered and shrunk for nothing.
  bool IsAvailable() const { return available_ && !failed_; }
  // NGX loaded and the GPU offers DLSS (needed by DLSS and Neural Rendering, not by SMAA).
  bool IsDlssSupported() const { return dlss_supported_; }
  // AMD FidelityFX loaded (FSR upscaling); tried on first use.
  bool IsFsrSupported();

  /**
   * Loads the Neural Rendering model through its caller-gate forwarder (both next to the
   * executable). Safe to call again; only the first call does anything.
   */
  bool InitializeNeuralRendering();

  /**
   * Records the whole pass on a command list. Leaves the colour and depth render targets in the
   * states given in the inputs, and the command list with no pipeline, root signature or
   * descriptor heap the caller can rely on.
   */
  void Record(ID3D12GraphicsCommandList* command_list, const Inputs& inputs);

 private:
  struct DlssKey {
    u32 width, height, render_width, render_height, quality, preset;
    bool hdr;
    bool operator==(const DlssKey&) const = default;
  };

  // chain_format: the working textures; color_format: the scene's own (scene_ldr_).
  bool EnsureTextures(const Inputs& inputs, DXGI_FORMAT chain_format, DXGI_FORMAT color_format);
  // width x height of the texture in `source_slot` at (x, y), times `scale`, into `dest_uav_slot`.
  void ScaleColor(ID3D12GraphicsCommandList* command_list, u32 source_slot, u32 x, u32 y,
                  u32 dest_uav_slot, u32 width, u32 height, f32 scale);
  bool InitializeSmaa();
  bool InitializeGi();
  void UploadSmaaTables(ID3D12GraphicsCommandList* command_list);
  // SMAA on width x height of `source` at (x, y), into color_out_ (left readable).
  void RunSmaa(ID3D12GraphicsCommandList* command_list, ID3D12Resource* source,
               D3D12_RESOURCE_STATES source_state, u32 x, u32 y, u32 width, u32 height);
  bool EnsureDlssFeature(ID3D12GraphicsCommandList* command_list, const Inputs& inputs);
  bool EnsureFsrContext(const Inputs& inputs);
  bool DispatchFsr(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                   ID3D12Resource* color);
  bool EnsureNrFeature(ID3D12GraphicsCommandList* command_list, const Inputs& inputs, u32 width,
                       u32 height);
  bool EvaluateNr(ID3D12GraphicsCommandList* command_list, const Inputs& inputs,
                  ID3D12Resource* color, u32 color_x, u32 color_y, u32 width, u32 height);
  void Compose(ID3D12GraphicsCommandList* command_list, const Inputs& inputs, u32 original_slot,
               u32 original_x, u32 original_y, u32 width, u32 height);
  void AgeRetiredFeatures(bool release_all);
  void Sharpen(ID3D12GraphicsCommandList* command_list, f32 sharpness, u32 source_slot,
               u32 source_x, u32 source_y, u32 width, u32 height);
  void SetParameterU32(const char* name, u32 value);
  void SetParameterF32(const char* name, f32 value);
  void SetParameterResource(const char* name, ID3D12Resource* resource);
  D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(u32 index) const;
  D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(u32 index) const;

  // Descriptor slots of heap_.
  enum : u32 {
    kSlotDepthSRV,     // inputs: the depth render target (t0, t1 contiguous)
    kSlotObjectMotionSRV,  // inputs: the scene motion texture
    kSlotDepthOutUAV,  // inputs: u0, u1 (contiguous)
    kSlotMotionOutUAV,
    kSlotSceneSRV,     // compose: the scene render target as the frame the model was given
    kSlotColorOutSRV,  // compose: DLSS's output as the frame the model was given
    kSlotNrOutUAV,     // compose: u0, u1 (u1 unused)
    kSlotStatsUAV,     // compose: u1, the statistics buffer
    kSlotSmaaColorSRV,   // SMAA: t0 of edges and blending (contiguous with the next)
    kSlotSmaaWeightsSRV, // SMAA: t1 of blending
    kSlotSmaaEdgesSRV,   // SMAA: t0, t1, t2 of the weights pass (contiguous)
    kSlotSmaaAreaSRV,
    kSlotSmaaSearchSRV,
    kSlotSmaaEdgesUAV,
    kSlotSmaaWeightsUAV,
    kSlotColorOutUAV,
    kSlotNrOutSRV,
    kSlotGiColorSRV,       // GI: t0, t1 (contiguous)
    kSlotGiDepthSRV,
    kSlotGiOutUAV,
    kSlotGiOutSRV,
    kSlotSharpenUAV,       // CAS: u0, u1 (u1 unused)
    kSlotSharpenUAVSpare,
    kSlotSceneHdrUAV,      // colour scale: u0, u1 (u1 unused)
    kSlotSceneHdrUAVSpare,
    kSlotSceneHdrSRV,
    kSlotSceneLdrUAV,      // colour scale back: u0, u1 (u1 unused)
    kSlotSceneLdrUAVSpare,
    kSlotScaleSourceSRV,   // colour scale back: the chain's result
    kSlotMotionCopyEdramUAV,  // motion copy: u0, u1 (contiguous)
    kSlotMotionCopyOutUAV,
    kSlotDebugOutUAV,  // motion debug: u0, u1 (contiguous)
    kSlotDebugStatsUAV,
    kSlotSpareSRV,  // so a two-descriptor table may start at the last slot
    kSlotCount,
  };

  ID3D12Device* device_ = nullptr;
  bool ngx_initialized_ = false;
  bool available_ = false;
  bool dlss_supported_ = false;
  bool failed_ = false;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  DlssKey feature_key_{};

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> inputs_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> cas_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> scale_pipeline_;
  // The scene as linear float (Inputs::color_scale above 1), and the result back in the scene's
  // own format before the copy into it.
  Microsoft::WRL::ComPtr<ID3D12Resource> scene_hdr_;
  Microsoft::WRL::ComPtr<ID3D12Resource> scene_ldr_;
  // Object motion of the scene in pixels (NaN where the camera motion applies), filled per tile.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> motion_copy_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> motion_debug_pipeline_;
  // The motion debug view's sums (see dlss_motion_debug_cs), read back and logged.
  static constexpr u32 kMotionStatsBytes = 32;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_stats_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_stats_zero_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_stats_readback_;
  u64 motion_stats_frame_ = 0;
  bool EnsureMotionStatsBuffers();
  void RecordMotionStats(ID3D12GraphicsCommandList* command_list);
  Microsoft::WRL::ComPtr<ID3D12Resource> scene_motion_;
  u32 scene_motion_width_ = 0;
  u32 scene_motion_height_ = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> gi_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> gi_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12Resource> gi_out_;
  u32 gi_frame_ = 0;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  u32 descriptor_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> color_out_;
  // A fixed exposure of 1 (1x1 R32_FLOAT) instead of DLSS's auto exposure.
  Microsoft::WRL::ComPtr<ID3D12Resource> exposure_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> exposure_rtv_heap_;
  bool exposure_ready_ = false;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_out_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_out_;
  Microsoft::WRL::ComPtr<ID3D12Resource> nr_out_;
  Microsoft::WRL::ComPtr<ID3D12Resource> sharpen_out_;
  // Copies of depth and motion for frame generation, so the presenter never reads a pair the
  // next frame is overwriting.
  static constexpr u32 kFrameGenerationSlots = 4;
  Microsoft::WRL::ComPtr<ID3D12Resource> fg_depth_[kFrameGenerationSlots];
  Microsoft::WRL::ComPtr<ID3D12Resource> fg_motion_[kFrameGenerationSlots];
  u32 fg_slot_ = 0;
  bool fg_textures_readable_ = false;  // created writable, rest readable after the first copy
  // SMAA.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> smaa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> smaa_pipelines_[3];
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_color_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_edges_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_weights_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_area_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_search_;
  Microsoft::WRL::ComPtr<ID3D12Resource> smaa_upload_;
  bool smaa_tables_uploaded_ = false;
  bool smaa_color_needs_transition_ = true;  // created writable, rests readable afterwards
  // How much the model changes the frame, summed by the compose shader and logged.
  bool EnsureStatsBuffers();
  void RecordStats(ID3D12GraphicsCommandList* command_list, u32 pixels);
  static constexpr u32 kStatsSlots = 4;
  Microsoft::WRL::ComPtr<ID3D12Resource> stats_;
  Microsoft::WRL::ComPtr<ID3D12Resource> stats_zero_;
  Microsoft::WRL::ComPtr<ID3D12Resource> stats_readback_;
  u64 stats_frame_ = 0;
  u32 stats_pixels_[kStatsSlots] = {};
  u32 textures_width_ = 0;
  u32 textures_height_ = 0;
  DXGI_FORMAT textures_chain_format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT textures_color_format_ = DXGI_FORMAT_UNKNOWN;

  // Neural Rendering, called through nvngx.dll_nr_forwarder.dll (the model checks its caller).
  using NrInit = int (*)(unsigned long long, const wchar_t*, void*, int, const void*);
  using NrCreate = int (*)(void*, int, const void*, void**);
  using NrEvaluate = int (*)(void*, const void*, const void*);
  using NrRelease = int (*)(void*);
  void* nr_forwarder_ = nullptr;  // HMODULE
  NrCreate nr_create_ = nullptr;
  NrEvaluate nr_evaluate_ = nullptr;
  NrRelease nr_release_ = nullptr;
  bool nr_initialize_attempted_ = false;
  bool nr_ready_ = false;
  bool nr_failed_ = false;
  int float_slot_ = -1;  // vtable slot of the parameter block's float setter
  void* nr_feature_ = nullptr;
  Inputs::NeuralRendering nr_feature_settings_;
  u32 nr_feature_width_ = 0;
  u32 nr_feature_height_ = 0;
  Inputs::NeuralRendering nr_pending_settings_;
  u32 nr_pending_frames_ = 0;

  // A feature freed while the GPU may still run it removes the device, so replaced features wait
  // this many passes before they are released.
  static constexpr u32 kRetireFrames = 32;
  struct RetiredFeature {
    NVSDK_NGX_Handle* dlss;
    void* nr;
    u32 frames_left;
    void* fsr = nullptr;  // ffxContext
  };

  // AMD FidelityFX API, loaded from amd_fidelityfx_loader_dx12.dll next to the executable.
  using FfxCreateContext = int32_t (*)(void** context, void* desc, const void* allocation);
  using FfxDestroyContext = int32_t (*)(void** context, const void* allocation);
  using FfxDispatch = int32_t (*)(void** context, const void* desc);
  void* ffx_loader_ = nullptr;  // HMODULE
  FfxCreateContext ffx_create_ = nullptr;
  FfxDestroyContext ffx_destroy_ = nullptr;
  FfxDispatch ffx_dispatch_ = nullptr;
  bool ffx_attempted_ = false;
  bool ffx_ready_ = false;
  void* fsr_context_ = nullptr;
  DlssKey fsr_key_{};
  std::vector<RetiredFeature> retired_;
};

}  // namespace rex::gpu::d3d12
