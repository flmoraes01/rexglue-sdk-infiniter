/**
 * @file        gpu/scene_camera.h
 * @brief       The title's scene camera per frame, for DLSS motion vectors in texture mode
 *
 * Some titles multiply their camera into each object's matrix on the CPU, so no shader constant
 * holds the view-projection. The title then submits it from its own camera code, tagged with the
 * number of frames it presented before (its VdSwap count); the GPU emulation reads the camera of
 * the frame it is processing by the swaps it processed, which runs behind the title.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * Stores the view-projection of frame `frame` (row vector: clip = position * matrix, row-major),
 * without any DLSS jitter. A later call for the same frame replaces it. Any thread.
 */
REX_API void SubmitSceneCamera(u64 frame, const std::array<f32, 16>& view_projection);

/** The view-projection submitted for `frame`, if it is still kept (the last 16 frames). */
REX_API bool GetSceneCamera(u64 frame, std::array<f32, 16>& view_projection_out);

}  // namespace rex::gpu
