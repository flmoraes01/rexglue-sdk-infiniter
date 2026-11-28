// Composes DLSS Neural Rendering's answer with the frame it was given, at the frame's resolution.
//
// The model ran on the prepared frame (dlss_nr_prepare_cs: the HDR correction and the model
// resolution). Below the frame's resolution only the model's change is taken, enlarged onto the
// full frame (a matched residual), so the frame keeps its own sharpness. The change is carried as a
// luminance ratio raised to a strength, so detail the model adds or removes is amplified without
// shifting brightness overall, and its colour is mixed in by a second strength; then the HDR
// correction is undone. Written for this project (the ratio composition follows the idea of
// RenoDX's DLSS 5 add-on, https://github.com/clshortfuse/renodx; no code is taken from it).
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssNrComposeCS
//            /Fh bytecode/d3d12_5_1/dlss_nr_compose_cs.h dlss_nr_compose_cs.hlsl

#include "dlss_nr_common.hlsli"

cbuffer constants : register(b0) {
  uint2 xe_original_origin;  // where the frame given to the model starts in its texture
  uint2 xe_size;             // frame size
  uint2 xe_model_size;       // the model resolution
  float xe_detail_strength;  // exponent on the luminance ratio, 1 = the model's own amount
  float xe_colour_strength;  // 0 keeps the frame's colour, 1 takes the model's
  float xe_max_ratio;        // bounds the ratio so a dark pixel cannot explode
  uint xe_debug_view;        // 0 composed, 1 amplified difference, 2 the model's raw answer
  uint xe_hdr_chain;
  uint xe_hdr_mode;
};

Texture2D<float4> xe_original : register(t0);  // the frame, in the chain's encoding
Texture2D<float4> xe_model_in : register(t1);  // what the model was given (sRGB, model size)
Texture2D<float4> xe_model : register(t2);     // the model's answer (sRGB, model size)
RWTexture2D<float4> xe_out : register(u0);     // the composed frame, in the chain's encoding
RWByteAddressBuffer xe_stats : register(u1);   // [0] sum of changes, [4] changed pixels

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  const float4 original_chain = xe_original.Load(int3(xe_original_origin + id.xy, 0));
  const float3 original = ChainToProxyLinear(original_chain.rgb, xe_hdr_chain, xe_hdr_mode);
  float3 model;
  if (all(xe_model_size == xe_size)) {
    model = SrgbToLinear(xe_model.Load(int3(id.xy, 0)).rgb);
  } else {
    // The model's change at this point, enlarged, on top of the full-resolution frame.
    const float2 uv = (float2(id.xy) + 0.5) / float2(xe_size);
    const float3 given = SrgbToLinear(LoadBilinear(xe_model_in, xe_model_size, uv).rgb);
    const float3 answer = SrgbToLinear(LoadBilinear(xe_model, xe_model_size, uv).rgb);
    model = max(original + (answer - given), 0.0);
  }
  // Statistics of the model's own change, read back on the CPU: summed largest-channel
  // difference in 1/255 steps (in the model's encoding), and the pixels it changed by more than
  // 2/255.
  const float3 change = abs(LinearToSrgb(model) - LinearToSrgb(original));
  const uint change_255 = uint(round(max(change.r, max(change.g, change.b)) * 255.0));
  uint ignored;
  xe_stats.InterlockedAdd(0, change_255, ignored);
  if (change_255 > 2) {
    xe_stats.InterlockedAdd(4, 1, ignored);
  }
  float3 result;
  if (xe_debug_view == 2) {
    result = model;
  } else if (xe_debug_view == 1) {
    result = saturate(change * 16.0);
  } else {
    const float original_luma = dot(original, kLuma);
    const float model_luma = dot(model, kLuma);
    float ratio = (model_luma + 1.0e-4) / (original_luma + 1.0e-4);
    ratio = clamp(pow(ratio, xe_detail_strength), 1.0 / xe_max_ratio, xe_max_ratio);
    const float3 lit = original * ratio;
    // The model's colour at the same luminance.
    const float3 model_at_luma =
        model * ((original_luma * ratio + 1.0e-4) / (model_luma + 1.0e-4));
    result = lerp(lit, model_at_luma, saturate(xe_colour_strength));
  }
  xe_out[id.xy] = float4(ProxyLinearToChain(result, xe_hdr_chain, xe_hdr_mode), original_chain.a);
}
