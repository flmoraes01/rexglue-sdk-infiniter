#pragma once

/**
 * @file        gpu/xenos/host/d3d12/streamline_runtime.h
 * @brief       NVIDIA Streamline for DLSS frame generation (DLSS-G, Multi Frame Generation), loaded
 *              from sl.interposer.dll next to the executable.
 *
 * Streamline is set up before the D3D12 device exists (manual hooking: only the DXGI factory used
 * for the swap chain is upgraded). Each presented frame gets a frame token; the presenter tags the
 * scene's depth and motion vectors, sets the camera constants, the PCL markers and the DLSS-G mode.
 */

#include <d3d12.h>
#include <dxgi1_4.h>

#include <chrono>

#include <rex/types.h>

#include "gpu/xenos/host/d3d12/frame_generation_exchange.h"

namespace rex::gpu::d3d12 {

class StreamlineRuntime {
 public:
  /** Loads and initializes Streamline. Call before creating the D3D12 device. */
  static bool Initialize();
  /** The runtime once Initialize succeeded, otherwise null. */
  static StreamlineRuntime* Get();
  static void Shutdown();

  void SetDevice(ID3D12Device* device);
  /** A Streamline proxy of the factory (new reference), or null on failure. */
  IDXGIFactory2* UpgradeFactory(IDXGIFactory2* factory);

  /** Whether DLSS-G is usable, and how many frames it can generate between rendered ones. */
  bool IsFrameGenerationAvailable() const { return dlssg_available_; }
  u32 GetMaxFramesToGenerate() const { return max_frames_to_generate_; }

  /**
   * Before Present: a new frame token, PCL markers up to render submit, and either the frame's
   * tags, constants and DLSS-G on with frames_to_generate, or DLSS-G off (frames_to_generate 0).
   */
  void BeginPresent(ID3D12GraphicsCommandList* command_list, const FrameGenerationFrame* frame,
                    u32 frames_to_generate, u32 display_width, u32 display_height);
  void EndPresent();

  /**
   * On the GPU command thread at the start of a title frame: a new frame token, the Reflex sleep
   * and the simulation and render submit start markers. Returns the token for the frame's inputs.
   */
  void* BeginTitleFrame();
  /** On the GPU command thread when the title frame is submitted (its swap). */
  void EndTitleFrame(void* token);

 private:
  bool InitializeInternal();

  void* module_ = nullptr;  // HMODULE
  bool dlssg_available_ = false;
  u32 max_frames_to_generate_ = 1;
  u32 frames_to_generate_set_ = UINT32_MAX;
  void* frame_token_ = nullptr;  // sl::FrameToken* used by the present being made
  void* last_tagged_token_ = nullptr;
  std::chrono::steady_clock::time_point state_logged_at_{};
  u32 state_presents_ = 0;
  u64 state_shown_ = 0;
};

}  // namespace rex::gpu::d3d12
