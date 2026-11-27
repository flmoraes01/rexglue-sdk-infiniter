/**
 * @file        base/output_painter.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/gpu/output_painter.h>
#include <rex/gpu/texture_hook.h>

#include <atomic>

namespace rex::gpu {

namespace {
std::atomic<IOutputPainter*> g_output_painter{nullptr};
std::atomic<ITextureLoadHook*> g_texture_load_hook{nullptr};
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

}  // namespace rex::gpu
