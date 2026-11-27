/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <utility>

#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_submission_tracker.h"
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/host/surface.h"
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::gpu::d3d12 {

class D3D12OverlayDrawContext final : public gpu::OverlayDrawContext {
 public:
  D3D12OverlayDrawContext(Presenter& presenter, u32 render_target_width, u32 render_target_height,
                          ID3D12GraphicsCommandList* command_list, UINT64 submission_index_current,
                          UINT64 submission_index_completed)
      : gpu::OverlayDrawContext(&presenter, render_target_width, render_target_height),
        command_list_(command_list),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed) {}

  ID3D12GraphicsCommandList* GetCommandList() const { return command_list_.Get(); }
  UINT64 GetSubmissionIndexCurrent() const { return submission_index_current_; }
  UINT64 GetSubmissionIndexCompleted() const { return submission_index_completed_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  UINT64 submission_index_current_;
  UINT64 submission_index_completed_;
};

class D3D12Presenter final : public Presenter {
  struct Key {};

 public:
  explicit D3D12Presenter(Key, HostGPULossCallback host_gpu_loss_callback,
                          const XenosGraphicsDriverBase& driver, const D3D12Provider& provider)
      : Presenter(std::move(host_gpu_loss_callback), driver), provider_(provider) {}

  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  /**
   * Format of the swap chain in SDR, and of every target the guest output and overlays paint to
   * (the SDR intermediate in HDR).
   */
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
  /** Swap chain format in HDR, encoded by the HDR10 pass. */
  static constexpr DXGI_FORMAT kSwapChainFormatHDR = DXGI_FORMAT_R10G10B10A2_UNORM;

  /** Refresh context whose callback must use the main direct queue of the provider. */
  class D3D12GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}

    /**
     * Returns the UAV capable output resource in kGuestOutputFormat.
     *
     * The callback starts in kGuestOutputInternalState and must transition back to it before
     * finishing.
     */
    ID3D12Resource* GetResourceUAVCapable() const { return resource_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  };

  static std::unique_ptr<D3D12Presenter> Create(HostGPULossCallback host_gpu_loss_callback,
                                                const XenosGraphicsDriverBase& driver,
                                                const D3D12Provider& provider) {
    auto presenter = std::make_unique<D3D12Presenter>(Key{}, std::move(host_gpu_loss_callback),
                                                      driver, provider);
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D12Presenter();

  const D3D12Provider& GetProvider() const { return provider_; }

  gpu::Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  void AwaitUISubmissionCompletionFromUIThread(UINT64 submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DriverDisconnectPaintingFromSurface() override;

  bool DriverRefreshGuestOutput(
      u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
      rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out) override;

  PaintResult DriverPaintAndPresent(bool execute_ui_drawers) override;
  /** Configures FSR frame generation and records its prepare pass for the frame being painted. */
  void PrepareFrameGeneration(ID3D12GraphicsCommandList* command_list);
  u32 GetDisplayRefreshHz() const;

 private:
  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        f32 x;
        f32 y;
      };
      f32 offset[2];
    };
    union {
      struct {
        f32 width;
        f32 height;
      };
      f32 size[2];
    };
  };

  enum class GuestOutputPaintRootParameter : u8 {
    kSource,
    kRectangle,
    kEffectConstants,

    kCount,
  };

  enum GuestOutputPaintRootSignatureIndex : u8 {
    kGuestOutputPaintRootSignatureIndexBilinear,

    kGuestOutputPaintRootSignatureCount,
  };

  static constexpr GuestOutputPaintRootSignatureIndex GetGuestOutputPaintRootSignatureIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintRootSignatureIndexBilinear;
      default:
        REX_UNHANDLED_CASE(effect);
        return kGuestOutputPaintRootSignatureCount;
    }
  }

  struct PaintContext : public rex::NonCopyable {
    explicit PaintContext() = default;

    static constexpr u32 kSwapChainBufferCount = 3;

    enum RTVIndex : u8 {
      kRTVIndexSwapChainBuffer0,
      kRTVIndexSDRIntermediate = kRTVIndexSwapChainBuffer0 + kSwapChainBufferCount,

      kRTVCount,
    };

    enum ViewIndex : u8 {
      kViewIndexGuestOutput0SRV,
      kViewIndexSDRIntermediateSRV = kViewIndexGuestOutput0SRV + kGuestOutputMailboxSize,

      kViewCount,
    };

    void AwaitSwapChainUsageCompletion() {
      present_submission_tracker.AwaitAllSubmissionsCompletion();
      paint_submission_tracker.AwaitAllSubmissionsCompletion();
    }

    void DestroySwapChain();

    /** Signaled before presenting. */
    D3D12SubmissionTracker paint_submission_tracker;
    /** Signaled after presenting. */
    D3D12SubmissionTracker present_submission_tracker;

    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kSwapChainBufferCount>
        command_allocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    /** Shader visible CBV/SRV/UAV heap. */
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view_heap;

    /**
     * Pairs of the paint submission index of last use and the nullable guest output texture.
     *
     * Indices are not mailbox indices. A reference missing from the array replaces the most
     * outdated one after awaiting its last paint usage.
     */
    std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
        guest_output_resource_paint_refs;

    u32 swap_chain_width = 0;
    u32 swap_chain_height = 0;
    bool swap_chain_allows_tearing = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    /** FidelityFX frame generation swap chain context (ffxContext) when the swap chain is AMD's. */
    void* frame_generation_swap_chain = nullptr;
    /** FidelityFX frame generation context, created once the scene pass publishes a frame. */
    void* frame_generation = nullptr;
    u32 frame_generation_display_width = 0;
    u32 frame_generation_display_height = 0;
    u32 frame_generation_max_render_width = 0;
    u32 frame_generation_max_render_height = 0;
    u64 frame_generation_frame_id = 0;
    bool frame_generation_was_fresh = false;
    std::chrono::steady_clock::time_point pacing_window_start{};
    u32 pacing_presents = 0;
    f64 pacing_present_ms = 0.0;
    u32 pacing_interval = 0;
    u32 pacing_flags = 0;
    void DestroyFrameGeneration();
    /** True when the swap chain is Streamline's (DLSS frame generation). */
    bool frame_generation_streamline = false;
    u32 frame_generation_streamline_frames = 0;
    bool frame_generation_streamline_unlimited = false;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount> swap_chain_buffers;

    /** True when the swap chain is kSwapChainFormatHDR and paints through the intermediate. */
    bool swap_chain_hdr = false;
    /** True when the display is in HDR mode, so the HDR10 pass encodes PQ. */
    bool display_hdr = false;
    /** The display's reported peak luminance in nits, 0 when unknown. */
    f32 display_max_nits = 0.0f;
    /**
     * kSwapChainFormat target the frame paints to before the HDR10 pass, in COMMON state between
     * paints. Swap chain sized.
     */
    Microsoft::WRL::ComPtr<ID3D12Resource> sdr_intermediate;
  };

  bool InitializeSurfaceIndependent();
  /** Encodes the SDR intermediate into the swap chain buffer and leaves it ready to present. */
  void PaintHDR10(ID3D12GraphicsCommandList* command_list, ID3D12Resource* sdr_intermediate,
                  ID3D12Resource* swap_chain_buffer,
                  D3D12_CPU_DESCRIPTOR_HANDLE swap_chain_buffer_rtv);
  /** Creates sdr_intermediate and its views at the swap chain size. */
  bool EnsureSDRIntermediate();

  const D3D12Provider& provider_;

  /** True when DXGI supports DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, used for variable refresh rate. */
  bool dxgi_supports_tearing_ = false;

  /** Destroyed only after awaiting main target usage completion. */
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, kGuestOutputPaintRootSignatureCount>
      guest_output_paint_root_signatures_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
             static_cast<size_t>(GuestOutputPaintEffect::kCount)>
      guest_output_paint_pipelines_;
  /** Writes the SDR intermediate to an HDR swap chain buffer. */
  Microsoft::WRL::ComPtr<ID3D12PipelineState> hdr10_pipeline_;

  /**
   * Pairs of the refresher fence value of the last refresh and the nullable guest output texture.
   *
   * Indexed by mailbox index.
   */
  std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
      guest_output_resources_;
  D3D12SubmissionTracker guest_output_resource_refresher_submission_tracker_;

  /** Submission tracker whose indices go to UI drawers, accessed from the UI thread only. */
  D3D12SubmissionTracker ui_submission_tracker_;

  /**
   * Accessed only by the thread that paints, manages the surface connection, or initializes and
   * shuts down the presenter.
   */
  PaintContext paint_context_;
};

}  // namespace rex::gpu::d3d12
