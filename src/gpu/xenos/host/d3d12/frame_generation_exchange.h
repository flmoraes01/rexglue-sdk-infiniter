#pragma once

/**
 * @file        gpu/xenos/host/d3d12/frame_generation_exchange.h
 * @brief       Hands each frame's depth, motion vectors and camera from the GPU command thread (the
 *              scene pass) to the presenter, which feeds them to frame generation before Present.
 */

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <mutex>

#include <rex/types.h>

namespace rex::gpu::d3d12 {

struct FrameGenerationFrame {
  u64 serial = 0;  // increases with every published frame
  // Held by reference: the presenter may still prepare a frame while the scene pass replaces or
  // frees its textures (shutdown, resolution change).
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;   // R32_FLOAT, render size, readable by shaders
  Microsoft::WRL::ComPtr<ID3D12Resource> motion;  // R16G16_FLOAT pixels to the previous frame
  u32 render_width = 0;
  u32 render_height = 0;
  f32 jitter_x = 0.0f;
  f32 jitter_y = 0.0f;
  f32 frame_time_ms = 16.7f;
  bool reset = false;
  f32 camera_near = 0.1f;
  f32 camera_far = 1000.0f;
  f32 camera_fov_y = 1.0f;
  f32 camera_position[3] = {};
  f32 camera_up[3] = {0.0f, 1.0f, 0.0f};
  f32 camera_right[3] = {1.0f, 0.0f, 0.0f};
  f32 camera_forward[3] = {0.0f, 0.0f, 1.0f};
  // Row-major, row vector (DLSS frame generation): view -> clip and back, and this frame's clip
  // to the previous frame's clip and back. Identity when unknown.
  f32 view_to_clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  f32 clip_to_view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  f32 clip_to_prev_clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  f32 prev_clip_to_clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::chrono::steady_clock::time_point published;
  // Streamline frame token (sl::FrameToken*) of the title frame this came from, or null.
  void* streamline_token = nullptr;
};

class FrameGenerationExchange {
 public:
  void Publish(const FrameGenerationFrame& frame) {
    std::lock_guard lock(mutex_);
    latest_ = frame;
    latest_.serial = ++serial_;
    latest_.published = std::chrono::steady_clock::now();
  }
  /** The latest frame; serial 0 when none was published yet. */
  FrameGenerationFrame Latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
  }

 private:
  mutable std::mutex mutex_;
  FrameGenerationFrame latest_;
  u64 serial_ = 0;
};

}  // namespace rex::gpu::d3d12
