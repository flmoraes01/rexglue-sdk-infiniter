/**
 * @file        base/output_painter.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/gpu/output_painter.h>
#include <rex/gpu/scene_camera.h>
#include <rex/gpu/texture_hook.h>

#include <atomic>
#include <mutex>

namespace rex::gpu {

namespace {
std::atomic<IOutputPainter*> g_output_painter{nullptr};
std::atomic<ITextureLoadHook*> g_texture_load_hook{nullptr};

struct SceneCameraSlot {
  u64 frame = UINT64_MAX;
  std::array<f32, 16> view_projection{};
};
std::mutex g_scene_camera_mutex;
std::array<SceneCameraSlot, 16> g_scene_cameras;
}  // namespace

void SetOutputPainter(IOutputPainter* painter) {
  g_output_painter.store(painter, std::memory_order_release);
}

IOutputPainter* GetOutputPainter() {
  return g_output_painter.load(std::memory_order_acquire);
}

void SetTextureLoadHook(ITextureLoadHook* hook) {
  g_texture_load_hook.store(hook, std::memory_order_release);
}

ITextureLoadHook* GetTextureLoadHook() {
  return g_texture_load_hook.load(std::memory_order_acquire);
}

void SubmitSceneCamera(u64 frame, const std::array<f32, 16>& view_projection) {
  std::lock_guard lock(g_scene_camera_mutex);
  g_scene_cameras[frame % g_scene_cameras.size()] = {frame, view_projection};
}

bool GetSceneCamera(u64 frame, std::array<f32, 16>& view_projection_out) {
  std::lock_guard lock(g_scene_camera_mutex);
  const SceneCameraSlot& slot = g_scene_cameras[frame % g_scene_cameras.size()];
  if (slot.frame != frame) {
    return false;
  }
  view_projection_out = slot.view_projection;
  return true;
}

}  // namespace rex::gpu
