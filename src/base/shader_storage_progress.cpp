#include <rex/gpu/shader_storage_progress.h>

namespace rex::gpu {

ShaderStorageProgress& GetShaderStorageProgress() {
  static ShaderStorageProgress progress;
  return progress;
}

}  // namespace rex::gpu
