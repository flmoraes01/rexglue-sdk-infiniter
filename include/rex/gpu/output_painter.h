/**
 * @file        gpu/output_painter.h
 * @brief       Hook that replaces how the presenter paints the title's output on the host surface
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

/**
 * One paint of the title's output by the D3D12 presenter. The painter records into the command
 * list; the presenter submits it.
 */
struct D3D12OutputPaint {
  ID3D12GraphicsCommandList* command_list = nullptr;
  /** The title's output: R10G10B10A2_UNORM, gamma applied, in PIXEL_SHADER_RESOURCE state, which
   * it must keep. */
  ID3D12Resource* source = nullptr;
  u32 source_width = 0;
  u32 source_height = 0;
  /** The back buffer, in RENDER_TARGET state, which it must keep. */
  ID3D12Resource* target = nullptr;
  /** D3D12_CPU_DESCRIPTOR_HANDLE::ptr of the back buffer's render target view. */
  u64 target_rtv = 0;
  /** DXGI_FORMAT of the back buffer. */
  u32 target_format = 0;
  /** The rectangle the output covers, in back buffer pixels. It may extend past the edges. */
  i32 output_x = 0;
  i32 output_y = 0;
  u32 output_width = 0;
  u32 output_height = 0;
  /** Index of the submission the command list belongs to, and the last one the GPU finished. A
   * resource the painter used in submission S can be released once completed >= S. */
  u64 submission = 0;
  u64 completed_submission = 0;
};

/**
 * Paints the title's output instead of the presenter's bilinear stretch, for upscalers and
 * post-processing. Called on the thread that paints, once per presented frame.
 */
class IOutputPainter {
 public:
  virtual ~IOutputPainter() = default;

  /**
   * @return True when it painted the output rectangle; false makes the presenter paint it as
   *         usual. The presenter restores its descriptor heaps, root signature and pipeline after
   *         the call, so the painter may change them.
   */
  virtual bool PaintD3D12(const D3D12OutputPaint& paint) = 0;
};

/**
 * Installs the painter, or removes it with nullptr. The painter must outlive its installation.
 * Any thread.
 */
REX_API void SetOutputPainter(IOutputPainter* painter);

/** Returns the installed painter or nullptr. Any thread. */
REX_API IOutputPainter* GetOutputPainter();

}  // namespace rex::gpu
