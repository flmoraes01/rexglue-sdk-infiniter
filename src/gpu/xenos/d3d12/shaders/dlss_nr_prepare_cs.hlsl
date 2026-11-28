// Prepares the frame for DLSS Neural Rendering: brings the chain's colour into the model's SDR
// range (dlss_nr_common.hlsli, the HDR correction) and down to the model resolution, as an
// sRGB-encoded 8-bit image. Each model pixel averages the frame pixels it covers.
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssNrPrepareCS
//            /Fh bytecode/d3d12_5_1/dlss_nr_prepare_cs.h dlss_nr_prepare_cs.hlsl

#include "dlss_nr_common.hlsli"

cbuffer constants : register(b0) {
  uint2 xe_origin;     // where the frame starts in its texture
  uint2 xe_size;       // frame size
  uint2 xe_model_size;
  uint xe_hdr_chain;   // the chain is linear HDR float (else sRGB-encoded SDR)
  uint xe_hdr_mode;    // HDR correction, see dlss_nr_common.hlsli
};

Texture2D<float4> xe_frame : register(t0);
RWTexture2D<unorm float4> xe_model_in : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_model_size)) {
    return;
  }
  // The frame pixels this model pixel covers, up to 3x3 of them (model resolution down to 1/3).
  const float2 scale = float2(xe_size) / float2(xe_model_size);
  const float2 start = float2(id.xy) * scale;
  const int2 first = int2(floor(start));
  const int2 last = min(int2(ceil(start + scale)) - 1, int2(xe_size) - 1);
  float3 sum = 0.0;
  float weight = 0.0;
  float alpha = 1.0;
  for (int y = first.y; y <= min(last.y, first.y + 2); ++y) {
    for (int x = first.x; x <= min(last.x, first.x + 2); ++x) {
      const float4 c = xe_frame.Load(int3(xe_origin + uint2(x, y), 0));
      sum += ChainToProxyLinear(c.rgb, xe_hdr_chain, xe_hdr_mode);
      alpha = c.a;
      weight += 1.0;
    }
  }
  xe_model_in[id.xy] = float4(LinearToSrgb(sum / max(weight, 1.0)), alpha);
}
