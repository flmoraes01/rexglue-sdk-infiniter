/**
 * @file        gpu/xenos/host/d3d12/d3d12_settings.h
 * @brief       Settings of the Direct3D 12 driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace rex::gpu::d3d12 {

/** The gpu.d3d12 settings. The D3D12 graphics driver owns one. */
class D3D12Settings : public SettingOwner {
 public:
  D3D12Settings() : SettingOwner("gpu.d3d12") {}

  REX_SETTING(i32, adapter, -1, "Adapter index, -1 picks the first hardware adapter, -2 picks WARP",
              Range{-2, 15}, kStartupOnly);
  REX_SETTING(bool, debug_markers, false,
              "Insert debug markers into GPU command streams for tools like PIX and RenderDoc. "
              "Turns on when RenderDoc is detected");
  REX_SETTING(bool, pass_timing, false,
              "Log the GPU time of each guest render pass (render target setup or resolve) every "
              "2 seconds, for profiling",
              kStartupOnly);
  REX_SETTING(bool, bindless, true, "Use bindless resources where available", kRestart);
  REX_SETTING(bool, readback_memexport, false,
              "Read data written by memory export in shaders on the CPU");
  REX_SETTING(bool, readback_resolve, false, "Read render to texture results on the CPU");
  REX_SETTING(bool, submit_on_primary_buffer_end, true,
              "Submit command list when PM4 primary buffer ends");
  REX_SETTING(bool, dxbc_disasm, false, "Dump DXBC disassembly");
  REX_SETTING(bool, dxbc_disasm_dxilconv, false, "Dump DXIL conversion disassembly");
  REX_SETTING(i32, pipeline_creation_threads, -1,
              "Number of pipeline creation threads, -1 for auto", Range{-1, 32}, kRestart);
  REX_SETTING(bool, tessellation_wireframe, false, "Render tessellation as wireframe");
  REX_SETTING(bool, native_stencil_value_output, true, "Enable native stencil value output");
  REX_SETTING(bool, use_fuzzy_alpha_epsilon, false,
              "Use approximate compare for alpha test values to prevent flickering on NVIDIA "
              "graphics cards");
  REX_SETTING(bool, dxbc_switch, true, "Use switch statements in DXBC");
  REX_SETTING(bool, dxbc_source_map, false, "Generate source maps for DXBC");
  REX_SETTING(bool, draw_resolution_scaled_texture_offsets, true,
              "Scale texture offsets with draw resolution");
  REX_SETTING(bool, native_stencil_value_output_intel, false,
              "Native stencil value output for Intel");
  REX_SETTING(std::string, render_target_path, "", "Render target implementation path",
              kStartupOnly);
  REX_SETTING(bool, tiled_shared_memory, true, "Use tiled shared memory", kRestart);
  REX_SETTING(bool, allow_tearing, true, "Allow variable refresh rate and tearing");
  REX_SETTING(bool, hdr, false,
              "HDR10 output on displays in HDR mode, expanding the SDR image with an inverse "
              "tone map",
              kRestart);
  REX_SETTING(f64, hdr_paper_white_nits, 200.0, "HDR brightness of SDR white (UI, midtones)",
              Range{80.0, 500.0});
  REX_SETTING(f64, hdr_peak_nits, 1000.0, "HDR brightest highlight", Range{200.0, 10000.0});
  REX_SETTING(f64, hdr_expansion, 4.0,
              "HDR highlight expansion curve, higher keeps more of the image at SDR levels",
              Range{1.0, 16.0});
  REX_SETTING(bool, debug, false, "Enable Direct3D 12 and DXGI debug layer", kStartupOnly);
  REX_SETTING(bool, break_on_error, false, "Break on Direct3D 12 validation errors");
  REX_SETTING(bool, break_on_warning, false, "Break on Direct3D 12 validation warnings");
  REX_SETTING(i32, queue_priority, 1,
              "Graphics command queue priority, 0 for normal, 1 for high, 2 for realtime",
              Range{0, 2});

  // NVIDIA DLSS. The title tells where its 3D scene ends and where its camera matrix lives.
  REX_SETTING(std::string, dlss, "off",
              "Scene anti-aliasing: off, smaa (any GPU), dlaa (NVIDIA, native resolution), or DLSS "
              "super resolution "
              "from a smaller scene: quality (67%), balanced (58%), performance (50%); needs an "
              "RTX GPU and nvngx_dlss.dll next to the executable");
  REX_SETTING(std::string, dlss_preset, "default",
              "DLSS model: default, cnn (the convolutional model), or transformer preset J, K, L "
              "or M");
  REX_SETTING(std::string, frame_generation, "off",
              "Frame generation: off, fsr (AMD, any GPU), or dlss / dlss_x3 / dlss_x4 (NVIDIA "
              "DLSS frame generation 2x, 3x, 4x; RTX 40/50). Switches at run time within AMD or "
              "NVIDIA; going from one to the other takes a restart (the swap chain is theirs)");
  REX_SETTING(bool, frame_generation_swap_chain, true,
              "Present through AMD's frame generation swap chain when FidelityFX is available, so "
              "frame generation can be switched at run time", kRestart);
  REX_SETTING(std::string, upscaler, "dlss",
              "Upscaler for the dlaa/quality/balanced/performance modes: dlss (NVIDIA RTX) or fsr "
              "(AMD FSR, any GPU; needs amd_fidelityfx_loader_dx12.dll and "
              "amd_fidelityfx_upscaler_dx12.dll next to the executable)");
  REX_SETTING(bool, ssgi, false,
              "Screen-space global illumination on the 3D scene: ambient occlusion and one bounce "
              "of indirect light from depth (not ray traced)");
  REX_SETTING(f64, ssgi_strength, 1.0, "Screen-space GI indirect light strength", Range{0.0, 4.0});
  REX_SETTING(f64, ssgi_ao_strength, 0.7, "Screen-space GI ambient occlusion strength",
              Range{0.0, 1.0});
  REX_SETTING(f64, ssgi_radius, 1.5, "Screen-space GI gather radius, in the game's units",
              Range{0.1, 20.0});
  REX_SETTING(f64, sharpening, 0.0,
              "AMD FidelityFX CAS sharpening of the 3D scene (before the HUD): 0 off, up to 1",
              Range{0.0, 1.0});
  REX_SETTING(bool, dlss_nr, false,
              "DLSS Neural Rendering (NVIDIA's generative detail model) on the 3D scene; needs "
              "nvngx_dlssnr.dll next to the executable");
  REX_SETTING(std::string, dlss_nr_style, "default", "Neural Rendering style: default, natural or "
              "cinematic");
  REX_SETTING(f64, dlss_nr_intensity, 1.0, "Neural Rendering intensity", Range{0.0, 2.0});
  REX_SETTING(f64, dlss_nr_local_structure, 1.0, "Neural Rendering local structure strength",
              Range{0.0, 2.0});
  REX_SETTING(f64, dlss_nr_local_tone, 1.0, "Neural Rendering local tone strength",
              Range{0.0, 2.0});
  REX_SETTING(f64, dlss_nr_skin_structure, -1.0,
              "Neural Rendering skin structure strength, -1 for the model's own", Range{-1.0, 2.0});
  REX_SETTING(std::string, dlss_nr_placement, "after",
              "Neural Rendering before or after DLSS: after (on DLSS's output) or before (on the "
              "rendered scene)");
  REX_SETTING(f64, dlss_nr_detail_strength, 1.5,
              "How much of the model's change in light is kept: 1 as the model gives it, higher "
              "amplifies it", Range{0.0, 4.0});
  REX_SETTING(f64, dlss_nr_colour_strength, 1.0,
              "How much of the model's colour is taken: 0 keeps the game's colour", Range{0.0, 1.0});
  REX_SETTING(std::string, dlss_nr_debug, "off",
              "Neural Rendering debug view: off, difference (what the model changed, x16) or "
              "model (its raw answer)");
  REX_SETTING(bool, dlss_jitter, true,
              "Shift the scene by a sub-pixel amount each frame for DLSS (off only to debug)");
  REX_SETTING(bool, dlss_jitter_invert, false,
              "Report the jitter to the upscaler with the opposite sign (debugging the "
              "convention; NVIDIA's and AMD's both take the offset applied to the camera)");
  REX_SETTING(std::string, dlss_scene_address, "",
              "Title: guest address (hex) of the resolve that ends the 3D scene, before "
              "post-processing and HUD");
  REX_SETTING(std::string, dlss_camera_shaders, "",
              "Title: comma-separated vertex shader ucode hashes (hex) that hold the camera's "
              "view-projection matrix");
  REX_SETTING(i32, dlss_camera_register, 0,
              "Title: first float constant of the view-projection matrix (4 rows, row vector)",
              Range{0, 252});
  REX_SETTING(std::string, dlss_scene_texture, "",
              "Texture mode for titles that draw the scene in tiles: WIDTHxHEIGHT:COLOR:DEPTH, the "
              "guest texture formats of the scene colour and depth the title loads before its "
              "post-processing. DLAA runs on the loaded colour texture; the camera comes from "
              "rex::gpu::SubmitSceneCamera");
  REX_SETTING(std::string, dlss_scene_target, "",
              "Texture mode: COLOR_FORMAT:MSAA of the render target the scene is drawn to, whose "
              "draws get the DLSS jitter");
};

}  // namespace rex::gpu::d3d12
