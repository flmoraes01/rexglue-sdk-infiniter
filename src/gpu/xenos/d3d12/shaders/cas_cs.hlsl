// AMD FidelityFX CAS (Contrast Adaptive Sharpening, thirdparty/fidelityfx_cas, MIT), sharpen only,
// on the scene before the HUD. Constants come from the same formulas as CasSetup().
//
// Build: fxc /nologo /T cs_5_0 /E main /O3 /Vn kCasCS /Fh bytecode/d3d12_5_1/cas_cs.h cas_cs.hlsl

cbuffer constants : register(b0) {
  uint4 xe_const0;
  uint4 xe_const1;
  uint2 xe_origin;  // where the frame starts in the source texture
  uint2 xe_size;
};

Texture2D<float4> xe_source : register(t0);
RWTexture2D<float4> xe_target : register(u0);

#define A_GPU 1
#define A_HLSL 1
#include "../../../../../thirdparty/fidelityfx_cas/ffx_a.h"

AF3 CasLoad(ASU2 p) {
  // CAS reads a 3x3 neighbourhood; clamp to the frame so the edges do not read outside it.
  const int2 q = clamp(p, int2(0, 0), int2(xe_size) - 1);
  return xe_source.Load(int3(int2(xe_origin) + q, 0)).rgb;
}
// The frame is sRGB encoded; CAS's documentation suggests squaring as a cheap gamma 2.0 linearise.
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {
  r *= r;
  g *= g;
  b *= b;
}

#include "../../../../../thirdparty/fidelityfx_cas/ffx_cas.h"

[numthreads(64, 1, 1)]
void main(uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {
  // One 16x16 tile per group, as in CAS's own example.
  AU2 gxy = ARmp8x8(local.x) + AU2(group.x << 4u, group.y << 4u);
  const AU2 steps[4] = {AU2(0u, 0u), AU2(8u, 0u), AU2(8u, 8u), AU2(0u, 8u)};
  [unroll] for (uint i = 0; i < 4; ++i) {
    const AU2 p = gxy + steps[i];
    if (all(p < xe_size)) {
      AF3 c;
      CasFilter(c.r, c.g, c.b, p, xe_const0, xe_const1, true);
      // Back to the encoding the frame was in.
      xe_target[p] = float4(sqrt(saturate(c)), 1.0);
    }
  }
}
