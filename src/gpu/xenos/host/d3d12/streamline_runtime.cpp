/**
 * @file        gpu/xenos/host/d3d12/streamline_runtime.cpp
 * @brief       NVIDIA Streamline for DLSS frame generation.
 */

#include "gpu/xenos/host/d3d12/streamline_runtime.h"

#include <windows.h>

#include <cstring>
#include <memory>
#include <string>

#include <rex/logging/log_category.h>

#if REX_STREAMLINE
#include <algorithm>
#include <type_traits>
#include <utility>
// The Streamline headers use MSVC pragmas and std::to_underlying without including <utility>.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
// Their C++23 branch is invalid (`using to_underlying = std::to_underlying`); the C++20 one,
// a small template of their own, is right, so these two headers see C++20.
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#define __cplusplus 202002L
#include <sl_pcl.h>
#include <sl_reflex.h>
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop
#endif

namespace rex::gpu::d3d12 {

#if REX_STREAMLINE
namespace {

struct Api {
  PFun_slInit* init = nullptr;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slSetD3DDevice* set_device = nullptr;
  PFun_slUpgradeInterface* upgrade = nullptr;
  PFun_slGetFeatureFunction* get_feature_function = nullptr;
  PFun_slGetNewFrameToken* new_frame_token = nullptr;
  PFun_slSetTagForFrame* set_tag = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
  PFun_slIsFeatureLoaded* is_feature_loaded = nullptr;
  PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
  PFun_slDLSSGGetState* dlssg_get_state = nullptr;
  PFun_slPCLSetMarker* pcl_marker = nullptr;
  PFun_slReflexSetOptions* reflex_set_options = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
};

Api g_api;
std::unique_ptr<StreamlineRuntime> g_runtime;

void LogMessage(sl::LogType type, const char* message) {
  if (type == sl::LogType::eError) {
    REXGPU_WARN("Streamline: {}", message);
  } else if (type == sl::LogType::eWarn) {
    REXGPU_INFO("Streamline: {}", message);
  }
}

sl::float4x4 ToFloat4x4(const f32 (&m)[16]) {
  sl::float4x4 out;
  for (u32 r = 0; r < 4; ++r) {
    out.setRow(r, sl::float4(m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]));
  }
  return out;
}

}  // namespace
#endif

bool StreamlineRuntime::Initialize() {
#if REX_STREAMLINE
  if (g_runtime) {
    return true;
  }
  auto runtime = std::make_unique<StreamlineRuntime>();
  if (!runtime->InitializeInternal()) {
    return false;
  }
  g_runtime = std::move(runtime);
  return true;
#else
  REXGPU_WARN("Streamline: not built in (configure the SDK with REXGLUE_STREAMLINE_SDK)");
  return false;
#endif
}

StreamlineRuntime* StreamlineRuntime::Get() {
#if REX_STREAMLINE
  return g_runtime.get();
#else
  return nullptr;
#endif
}

void StreamlineRuntime::Shutdown() {
#if REX_STREAMLINE
  if (g_runtime && g_api.shutdown) {
    g_api.shutdown();
  }
  g_runtime.reset();
#endif
}

bool StreamlineRuntime::InitializeInternal() {
#if REX_STREAMLINE
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring directory(exe_path);
  directory.resize(directory.find_last_of(L"\\/") + 1);
  HMODULE module = LoadLibraryW((directory + L"sl.interposer.dll").c_str());
  if (!module) {
    REXGPU_WARN("Streamline: sl.interposer.dll is missing next to the executable");
    return false;
  }
  module_ = module;
  auto load = [&](auto& out, const char* name) {
    out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(GetProcAddress(module, name));
    return out != nullptr;
  };
  if (!load(g_api.init, "slInit") || !load(g_api.shutdown, "slShutdown") ||
      !load(g_api.set_device, "slSetD3DDevice") || !load(g_api.upgrade, "slUpgradeInterface") ||
      !load(g_api.get_feature_function, "slGetFeatureFunction") ||
      !load(g_api.new_frame_token, "slGetNewFrameToken") ||
      !load(g_api.set_tag, "slSetTagForFrame") || !load(g_api.set_constants, "slSetConstants") ||
      !load(g_api.is_feature_loaded, "slIsFeatureLoaded")) {
    REXGPU_WARN("Streamline: sl.interposer.dll lacks its entry points");
    return false;
  }
  static const sl::Feature kFeatures[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                          sl::kFeaturePCL};
  static const std::wstring plugin_path = directory;
  static const wchar_t* plugin_paths[] = {plugin_path.c_str()};
  sl::Preferences preferences;
  preferences.showConsole = false;
  preferences.logLevel = sl::LogLevel::eDefault;
  preferences.logMessageCallback = &LogMessage;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  // Only the swap chain's factory is upgraded; nothing is downloaded at run time.
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseManualHooking |
                      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.featuresToLoad = kFeatures;
  preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(kFeatures));
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "1.0";
  preferences.projectId = "5a1d8d3e-7c2b-4f0e-9d61-1d2f0e3c4b5a";
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result result = g_api.init(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    REXGPU_WARN("Streamline: slInit failed ({})", static_cast<u32>(result));
    return false;
  }
  REXGPU_INFO("Streamline: initialized");
  return true;
#else
  return false;
#endif
}

void StreamlineRuntime::SetDevice(ID3D12Device* device) {
#if REX_STREAMLINE
  if (g_api.set_device(device) != sl::Result::eOk) {
    REXGPU_WARN("Streamline: slSetD3DDevice failed, DLSS frame generation is off");
    return;
  }
  bool loaded = false;
  if (g_api.is_feature_loaded(sl::kFeatureDLSS_G, loaded) != sl::Result::eOk || !loaded) {
    REXGPU_WARN("Streamline: DLSS frame generation is not available on this GPU or driver");
    return;
  }
  void* function = nullptr;
  auto feature_function = [&](sl::Feature feature, const char* name) -> void* {
    function = nullptr;
    return g_api.get_feature_function(feature, name, function) == sl::Result::eOk ? function
                                                                                   : nullptr;
  };
  g_api.dlssg_set_options = reinterpret_cast<PFun_slDLSSGSetOptions*>(
      feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions"));
  g_api.dlssg_get_state = reinterpret_cast<PFun_slDLSSGGetState*>(
      feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState"));
  g_api.pcl_marker =
      reinterpret_cast<PFun_slPCLSetMarker*>(feature_function(sl::kFeaturePCL, "slPCLSetMarker"));
  g_api.reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions*>(
      feature_function(sl::kFeatureReflex, "slReflexSetOptions"));
  g_api.reflex_sleep = reinterpret_cast<PFun_slReflexSleep*>(
      feature_function(sl::kFeatureReflex, "slReflexSleep"));
  if (!g_api.dlssg_set_options || !g_api.dlssg_get_state || !g_api.pcl_marker ||
      !g_api.reflex_set_options) {
    REXGPU_WARN("Streamline: DLSS frame generation functions are missing");
    return;
  }
  // DLSS frame generation needs Reflex on.
  sl::ReflexOptions reflex;
  // DLSS frame generation requires Reflex. Its markers follow the title's frame on the GPU
  // command thread (BeginTitleFrame / EndTitleFrame), where the work is really submitted.
  reflex.mode = sl::ReflexMode::eLowLatency;
  g_api.reflex_set_options(reflex);
  sl::DLSSGState state;
  if (g_api.dlssg_get_state(sl::ViewportHandle(0), state, nullptr) == sl::Result::eOk &&
      state.numFramesToGenerateMax > 0) {
    max_frames_to_generate_ = state.numFramesToGenerateMax;
  }
  dlssg_available_ = true;
  REXGPU_INFO("Streamline: DLSS frame generation available (up to {}x)",
              max_frames_to_generate_ + 1);
#else
  (void)device;
#endif
}

IDXGIFactory2* StreamlineRuntime::UpgradeFactory(IDXGIFactory2* factory) {
#if REX_STREAMLINE
  void* proxy = factory;
  factory->AddRef();
  if (g_api.upgrade(&proxy) != sl::Result::eOk || !proxy) {
    factory->Release();
    REXGPU_WARN("Streamline: cannot upgrade the DXGI factory");
    return nullptr;
  }
  return static_cast<IDXGIFactory2*>(proxy);
#else
  (void)factory;
  return nullptr;
#endif
}

void* StreamlineRuntime::BeginTitleFrame() {
#if REX_STREAMLINE
  if (!dlssg_available_) {
    return nullptr;
  }
  sl::FrameToken* token = nullptr;
  if (g_api.new_frame_token(token, nullptr) != sl::Result::eOk || !token) {
    return nullptr;
  }
  // The sleep paces the title's GPU submission, where Reflex expects it; the title's CPU work
  // runs on its own threads and waits on the GPU as it would on the console.
  g_api.reflex_sleep(*token);
  g_api.pcl_marker(sl::PCLMarker::eSimulationStart, *token);
  g_api.pcl_marker(sl::PCLMarker::eSimulationEnd, *token);
  g_api.pcl_marker(sl::PCLMarker::eRenderSubmitStart, *token);
  return token;
#else
  return nullptr;
#endif
}

void StreamlineRuntime::EndTitleFrame(void* token) {
#if REX_STREAMLINE
  if (token && dlssg_available_) {
    g_api.pcl_marker(sl::PCLMarker::eRenderSubmitEnd, *static_cast<sl::FrameToken*>(token));
  }
#else
  (void)token;
#endif
}

void StreamlineRuntime::BeginPresent(ID3D12GraphicsCommandList* command_list,
                                     const FrameGenerationFrame* frame, u32 frames_to_generate,
                                     u32 display_width, u32 display_height) {
#if REX_STREAMLINE
  // The token of the title frame being shown; a present without one (no 3D scene) gets its own.
  sl::FrameToken* token = frame ? static_cast<sl::FrameToken*>(frame->streamline_token) : nullptr;
  if (!token) {
    if (g_api.new_frame_token(token, nullptr) != sl::Result::eOk || !token) {
      frame_token_ = nullptr;
      return;
    }
  }
  frame_token_ = token;
  if (!dlssg_available_) {
    return;
  }
  const sl::ViewportHandle viewport(0);
  // A title frame shown a second time keeps the tags and constants it already has.
  const bool new_frame = token != last_tagged_token_;
  if (frame && frames_to_generate > 0 && new_frame) {
    last_tagged_token_ = token;
    sl::Resource depth(sl::ResourceType::eTex2d, frame->depth.Get(), nullptr, nullptr,
                       static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    sl::Resource motion(sl::ResourceType::eTex2d, frame->motion.Get(), nullptr, nullptr,
                        static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    sl::Extent extent{0, 0, frame->render_width, frame->render_height};
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                        &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent, &extent)};
    g_api.set_tag(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)),
                  reinterpret_cast<sl::CommandBuffer*>(command_list));

    sl::Constants constants;
    constants.cameraViewToClip = ToFloat4x4(frame->view_to_clip);
    constants.clipToCameraView = ToFloat4x4(frame->clip_to_view);
    constants.clipToPrevClip = ToFloat4x4(frame->clip_to_prev_clip);
    constants.prevClipToClip = ToFloat4x4(frame->prev_clip_to_clip);
    constants.jitterOffset = sl::float2(frame->jitter_x, frame->jitter_y);
    // Motion vectors are in rendered pixels; DLSS-G wants them in [-1, 1].
    constants.mvecScale = sl::float2(1.0f / static_cast<f32>(frame->render_width),
                                     1.0f / static_cast<f32>(frame->render_height));
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(frame->camera_position[0], frame->camera_position[1],
                                     frame->camera_position[2]);
    constants.cameraUp =
        sl::float3(frame->camera_up[0], frame->camera_up[1], frame->camera_up[2]);
    constants.cameraRight =
        sl::float3(frame->camera_right[0], frame->camera_right[1], frame->camera_right[2]);
    constants.cameraFwd = sl::float3(frame->camera_forward[0], frame->camera_forward[1],
                                     frame->camera_forward[2]);
    constants.cameraNear = frame->camera_near;
    constants.cameraFar = frame->camera_far;
    constants.cameraFOV = frame->camera_fov_y;
    constants.cameraAspectRatio =
        static_cast<f32>(display_width) / static_cast<f32>(display_height ? display_height : 1);
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    g_api.set_constants(constants, *token, viewport);
  }
  const u32 wanted = frame ? std::min(frames_to_generate, max_frames_to_generate_) : 0;
  if (wanted != frames_to_generate_set_) {
    sl::DLSSGOptions options;
    options.mode = wanted ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = wanted ? wanted : 1;
    const sl::Result result = g_api.dlssg_set_options(viewport, options);
    REXGPU_INFO("Streamline: DLSS frame generation {} ({})",
                wanted ? std::to_string(wanted + 1) + "x" : std::string("off"),
                static_cast<u32>(result));
    frames_to_generate_set_ = wanted;
  }
  g_api.pcl_marker(sl::PCLMarker::ePresentStart, *token);
  // Every 2 seconds: what DLSS-G really shows (generated frames included) and its status.
  const auto now = std::chrono::steady_clock::now();
  ++state_presents_;
  if (now - state_logged_at_ >= std::chrono::seconds(2)) {
    sl::DLSSGState state;
    if (g_api.dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
      state_shown_ += state.numFramesActuallyPresented;
      const f64 seconds = std::chrono::duration<f64>(now - state_logged_at_).count();
      if (state_logged_at_.time_since_epoch().count()) {
        REXGPU_INFO("Streamline: DLSS-G status {}, {:.1f} frames shown/s for {:.1f} presents/s",
                    static_cast<u32>(state.status), state_shown_ / seconds,
                    state_presents_ / seconds);
      }
    }
    state_logged_at_ = now;
    state_presents_ = 0;
    state_shown_ = 0;
  } else {
    sl::DLSSGState state;
    if (g_api.dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
      state_shown_ += state.numFramesActuallyPresented;
    }
  }
#else
  (void)command_list;
  (void)frame;
  (void)frames_to_generate;
  (void)display_width;
  (void)display_height;
#endif
}

void StreamlineRuntime::EndPresent() {
#if REX_STREAMLINE
  if (frame_token_ && dlssg_available_) {
    g_api.pcl_marker(sl::PCLMarker::ePresentEnd, *static_cast<sl::FrameToken*>(frame_token_));
  }
#endif
}

}  // namespace rex::gpu::d3d12
