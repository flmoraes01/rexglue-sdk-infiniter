// Final presentation pass for HDR output. Reads the finished SDR frame (guest output and overlays,
// sRGB encoded) and writes HDR10: Rec.2020 primaries, SMPTE ST 2084 (PQ) transfer.
//
// SDR content maps to paper white; an inverse tone map expands the brightest values toward the
// display's peak so highlights gain range while midtones and the UI stay at their SDR look.
//
// Build: fxc /nologo /T ps_5_1 /E main /O3 /Vn kGuestOutputHdr10PS
//            /Fh bytecode/d3d12_5_1/guest_output_hdr10_ps.h guest_output_hdr10_ps.hlsl
// The root signature matches the bilinear guest output paint one (t0, s0, b0 with 4 values).

cbuffer push_consts_xe : register(b0) {
  float xe_hdr_paper_white_nits;  // SDR white level
  float xe_hdr_peak_nits;         // brightest output
  float xe_hdr_expansion;         // 0 = no expansion, higher = stronger highlights
  float xe_hdr_enabled;           // 0 = pass the SDR frame through unchanged
};
Texture2D<float4> xe_hdr_source : register(t0);

float3 SrgbToLinear(float3 c) {
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 Rec709ToRec2020(float3 c) {
  return float3(dot(float3(0.6274040, 0.3292820, 0.0433136), c),
                dot(float3(0.0690970, 0.9195400, 0.0113612), c),
                dot(float3(0.0163916, 0.0880132, 0.8955950), c));
}

float3 LinearToPQ(float3 nits) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float3 y = pow(saturate(nits / 10000.0), m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(float4 xe_position : SV_Position) : SV_Target {
  float3 color = xe_hdr_source.Load(int3(int2(xe_position.xy), 0)).rgb;
  if (xe_hdr_enabled == 0.0) {
    return float4(color, 1.0);
  }
  float3 linear_color = SrgbToLinear(saturate(color));
  // Expand by luminance so hue is kept: scale grows from 1 at black to peak / paper white at
  // SDR white, rising steeply only near the top of the range.
  float luminance = dot(float3(0.2126, 0.7152, 0.0722), linear_color);
  float headroom = max(xe_hdr_peak_nits / xe_hdr_paper_white_nits, 1.0);
  float scale = 1.0 + (headroom - 1.0) * pow(saturate(luminance), xe_hdr_expansion);
  float3 nits = Rec709ToRec2020(linear_color) * scale * xe_hdr_paper_white_nits;
  return float4(LinearToPQ(nits), 1.0);
}
