/**
 * @file        gpu/texture_hook.h
 * @brief       Hook that sees, and may replace, textures the GPU emulation loads from guest memory
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rex::gpu {

/** A texture the D3D12 texture cache just loaded from guest memory, at its place in the frame. */
struct D3D12TextureLoad {
  /** The command list of the submission, at the point right after the load. */
  ID3D12GraphicsCommandList* command_list = nullptr;
  /** The host texture, in COPY_DEST state, which it must keep. The hook may copy into it. */
  ID3D12Resource* texture = nullptr;
  u32 width = 0;   /**< Guest width; the host texture is larger with resolution scaling. */
  u32 height = 0;  /**< Guest height. */
  u32 guest_format = 0;  /**< xenos::TextureFormat. */
  u32 base_page = 0;     /**< Physical 4 KB page of the guest data. */
  /** Index of the submission and the last one the GPU finished, for resource lifetimes. */
  u64 submission = 0;
  u64 completed_submission = 0;
};

/**
 * Receives textures matching its filter as they load. Runs on the GPU emulation thread; the
 * filter runs when the load is recorded and the callback when the command list is built.
 */
class ITextureLoadHook {
 public:
  virtual ~ITextureLoadHook() = default;

  /** Whether a load of this guest texture should reach OnTextureLoadedD3D12. Must be cheap. */
  virtual bool WantsTexture(u32 width, u32 height, u32 guest_format) = 0;

  /**
   * Records commands into paint.command_list. The emulation rebinds its own state afterwards, so
   * the hook may change pipelines, root signatures, descriptor heaps, viewports and render
   * targets; it must leave every resource it did not create in the state it found it in.
   */
  virtual void OnTextureLoadedD3D12(const D3D12TextureLoad& load) = 0;
};

/** Installs the hook, or removes it with nullptr. It must outlive its installation. Any thread. */
REX_API void SetTextureLoadHook(ITextureLoadHook* hook);

/** Returns the installed hook or nullptr. Any thread. */
REX_API ITextureLoadHook* GetTextureLoadHook();

}  // namespace rex::gpu
