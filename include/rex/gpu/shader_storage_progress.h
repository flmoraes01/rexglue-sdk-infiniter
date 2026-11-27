/**
 * @file        gpu/shader_storage_progress.h
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * Progress of loading the persistent shader storage at driver start: shaders translated from the
 * stored ucode, then pipelines created from the stored descriptions. Written by the driver, read
 * from any thread, for a "preparing shaders" display.
 */
struct ShaderStorageProgress {
  std::atomic<bool> active{false};       /**< True while the storage is loading. */
  std::atomic<u32> shaders_queued{0};    /**< Stored shaders read so far. */
  std::atomic<u32> shaders_done{0};      /**< Of those, translated (or failed). */
  std::atomic<u32> pipelines_total{0};   /**< Stored pipelines to create, known once shaders end. */
  std::atomic<u32> pipelines_done{0};    /**< Of those, created. */
};

/** Returns the process's shader storage progress. In rexbase, so a title reads it too. */
REX_API ShaderStorageProgress& GetShaderStorageProgress();

}  // namespace rex::gpu
