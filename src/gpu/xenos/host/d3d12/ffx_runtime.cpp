/**
 * @file        gpu/xenos/host/d3d12/ffx_runtime.cpp
 * @brief       AMD FidelityFX API entry points, loaded once.
 */

#include "gpu/xenos/host/d3d12/ffx_runtime.h"

#include <windows.h>

#include <string>

#include <rex/logging/log_category.h>

namespace rex::gpu::d3d12 {

const FfxRuntime* GetFfxRuntime() {
  static const FfxRuntime* runtime = []() -> const FfxRuntime* {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring directory(exe_path);
    directory.resize(directory.find_last_of(L"\\/") + 1);
    HMODULE loader = LoadLibraryW((directory + L"amd_fidelityfx_loader_dx12.dll").c_str());
    if (!loader) {
      REXGPU_WARN("FidelityFX: amd_fidelityfx_loader_dx12.dll is missing next to the executable");
      return nullptr;
    }
    static FfxRuntime loaded;
    loaded.create = reinterpret_cast<FfxRuntime::Create>(GetProcAddress(loader, "ffxCreateContext"));
    loaded.destroy =
        reinterpret_cast<FfxRuntime::Destroy>(GetProcAddress(loader, "ffxDestroyContext"));
    loaded.configure =
        reinterpret_cast<FfxRuntime::Configure>(GetProcAddress(loader, "ffxConfigure"));
    loaded.query = reinterpret_cast<FfxRuntime::Query>(GetProcAddress(loader, "ffxQuery"));
    loaded.dispatch =
        reinterpret_cast<FfxRuntime::Dispatch>(GetProcAddress(loader, "ffxDispatch"));
    if (!loaded.create || !loaded.destroy || !loaded.configure || !loaded.query ||
        !loaded.dispatch) {
      REXGPU_WARN("FidelityFX: the loader lacks its entry points");
      return nullptr;
    }
    REXGPU_INFO("FidelityFX: API loaded");
    return &loaded;
  }();
  return runtime;
}

}  // namespace rex::gpu::d3d12
