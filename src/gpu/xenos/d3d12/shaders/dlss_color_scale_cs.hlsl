// Scales a colour texture into another: the scene's fixed-point colour (stored as linear colour
// times a resolve exponent bias) into linear float for DLSS, and DLSS's result back. A unorm
// destination clamps to [0, 1] like the console's resolve does.
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssColorScaleCS
//            /Fh bytecode/d3d12_5_1/dlss_color_scale_cs.h dlss_color_scale_cs.hlsl

cbuffer constants : register(b0) {
  uint2 xe_origin;  // where the source starts, in pixels
  uint2 xe_size;    // size to scale, in pixels
  float xe_scale;
};

Texture2D<float4> xe_source : register(t0);
RWTexture2D<float4> xe_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  xe_dest[id.xy] = xe_source.Load(int3(xe_origin + id.xy, 0)) * xe_scale;
}
