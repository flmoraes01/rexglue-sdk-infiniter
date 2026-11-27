// Composes DLSS Neural Rendering's answer with the frame it was given. The model's change to an
// SDR frame is subtle; it is carried as a luminance ratio raised to a strength, so detail the model
// adds or removes is amplified without shifting brightness overall, and its colour is mixed in by a
// second strength. Written for this project (the idea of a ratio composition follows RenoDX's DLSS 5
// add-on, https://github.com/clshortfuse/renodx; no code is taken from it).
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssNrComposeCS
//            /Fh bytecode/d3d12_5_1/dlss_nr_compose_cs.h dlss_nr_compose_cs.hlsl

cbuffer constants : register(b0) {
  uint2 xe_original_origin;  // where the frame given to the model starts in its texture
  uint2 xe_size;
  float xe_detail_strength;  // exponent on the luminance ratio, 1 = the model's own amount
  float xe_colour_strength;  // 0 keeps the frame's colour, 1 takes the model's
  float xe_max_ratio;        // bounds the ratio so a dark pixel cannot explode
  uint xe_debug_view;        // 0 composed, 1 amplified difference, 2 the model's raw answer
};

Texture2D<float4> xe_original : register(t0);
RWTexture2D<float4> xe_model : register(u0);  // the model's answer in, the composed frame out
RWByteAddressBuffer xe_stats : register(u1);   // [0] sum of changes, [4] changed pixels

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 SrgbToLinear(float3 c) {
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToSrgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  const float4 original_encoded = xe_original.Load(int3(xe_original_origin + id.xy, 0));
  const float4 model_encoded = xe_model[id.xy];
  // Statistics of the model's own change, read back on the CPU: summed largest-channel
  // difference in 1/255 steps, and the number of pixels it changed by more than 2/255.
  const float3 change = abs(model_encoded.rgb - original_encoded.rgb);
  const uint change_255 = uint(round(max(change.r, max(change.g, change.b)) * 255.0));
  uint ignored;
  xe_stats.InterlockedAdd(0, change_255, ignored);
  if (change_255 > 2) {
    xe_stats.InterlockedAdd(4, 1, ignored);
  }
  if (xe_debug_view == 2) {
    return;  // leave the model's answer as it is
  }
  if (xe_debug_view == 1) {
    xe_model[id.xy] = float4(saturate(abs(model_encoded.rgb - original_encoded.rgb) * 16.0), 1.0);
    return;
  }
  const float3 original = SrgbToLinear(original_encoded.rgb);
  const float3 model = SrgbToLinear(model_encoded.rgb);
  const float original_luma = dot(original, kLuma);
  const float model_luma = dot(model, kLuma);
  float ratio = (model_luma + 1.0e-4) / (original_luma + 1.0e-4);
  ratio = clamp(pow(ratio, xe_detail_strength), 1.0 / xe_max_ratio, xe_max_ratio);
  const float3 lit = original * ratio;
  // The model's colour at the same luminance.
  const float3 model_at_luma = model * ((original_luma * ratio + 1.0e-4) / (model_luma + 1.0e-4));
  const float3 result = lerp(lit, model_at_luma, saturate(xe_colour_strength));
  xe_model[id.xy] = float4(LinearToSrgb(result), original_encoded.a);
}
