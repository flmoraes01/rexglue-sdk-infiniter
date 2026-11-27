// SMAA 1x (Jimenez et al., thirdparty/smaa, MIT) as three compute passes over the scene:
//   SMAA_PASS 0: luma edge detection     t0 = colour            -> u0 = edges (RG)
//   SMAA_PASS 1: blending weights        t0 = edges, t1 = area, t2 = search -> u0 = weights (RGBA)
//   SMAA_PASS 2: neighbourhood blending  t0 = colour, t1 = weights          -> u0 = result
//
// Build (from this directory):
//   fxc /nologo /T cs_5_0 /E main /O3 /D SMAA_PASS=0 /Vn kSmaaEdgesCS
//       /Fh bytecode/d3d12_5_1/smaa_edges_cs.h smaa_cs.hlsl
//   fxc /nologo /T cs_5_0 /E main /O3 /D SMAA_PASS=1 /Vn kSmaaWeightsCS
//       /Fh bytecode/d3d12_5_1/smaa_weights_cs.h smaa_cs.hlsl
//   fxc /nologo /T cs_5_0 /E main /O3 /D SMAA_PASS=2 /Vn kSmaaBlendCS
//       /Fh bytecode/d3d12_5_1/smaa_blend_cs.h smaa_cs.hlsl

cbuffer constants : register(b0) {
  float4 xe_rt_metrics;  // 1 / width, 1 / height, width, height
  uint2 xe_size;
};

#define SMAA_RT_METRICS xe_rt_metrics
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
// Compute shaders cannot discard; the edge passes return "no edge" instead.
#define discard return float2(0.0, 0.0)
#include "../../../../../thirdparty/smaa/SMAA.hlsl"
#undef discard

Texture2D xe_t0 : register(t0);
Texture2D xe_t1 : register(t1);
Texture2D xe_t2 : register(t2);
RWTexture2D<float4> xe_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  const float2 texcoord = (float2(id.xy) + 0.5) * xe_rt_metrics.xy;
#if SMAA_PASS == 0
  float4 offset[3];
  SMAAEdgeDetectionVS(texcoord, offset);
  xe_out[id.xy] = float4(SMAALumaEdgeDetectionPS(texcoord, offset, xe_t0), 0.0, 0.0);
#elif SMAA_PASS == 1
  float2 pixcoord;
  float4 offset[3];
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
  xe_out[id.xy] =
      SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, xe_t0, xe_t1, xe_t2, 0.0);
#else
  float4 offset;
  SMAANeighborhoodBlendingVS(texcoord, offset);
  xe_out[id.xy] = SMAANeighborhoodBlendingPS(texcoord, offset, xe_t0, xe_t1);
#endif
}
