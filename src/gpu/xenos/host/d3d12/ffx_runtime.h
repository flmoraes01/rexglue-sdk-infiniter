#pragma once

/**
 * @file        gpu/xenos/host/d3d12/ffx_runtime.h
 * @brief       AMD FidelityFX API entry points, loaded once from amd_fidelityfx_loader_dx12.dll next
 *              to the executable (FSR upscaling and frame generation).
 */

#include <cstdint>

namespace rex::gpu::d3d12 {

struct FfxRuntime {
  using Create = int32_t (*)(void** context, void* desc, const void* allocation);
  using Destroy = int32_t (*)(void** context, const void* allocation);
  using Configure = int32_t (*)(void** context, const void* desc);
  using Query = int32_t (*)(void** context, void* desc);
  using Dispatch = int32_t (*)(void** context, const void* desc);
  Create create = nullptr;
  Destroy destroy = nullptr;
  Configure configure = nullptr;
  Query query = nullptr;
  Dispatch dispatch = nullptr;
};

/** The loaded API, or null when the loader is missing (logged once). Thread safe. */
const FfxRuntime* GetFfxRuntime();

}  // namespace rex::gpu::d3d12
