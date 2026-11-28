// Shared by the DLSS Neural Rendering prepare and compose passes: the colour spaces the model sees.
//
// The model works on display-referred SDR frames (sRGB-encoded 8-bit). A linear HDR chain (a
// float scene, such as a fixed-point scene texture scaled back up) is brought into that range by a
// reversible curve, the HDR correction, and taken back after the model:
//   0 clip:      values above 1 are lost (as an SDR game would have them)
//   1 soft knee: x / (1 + x) on every value, so highlights keep their gradation
//   2 hybrid:    unchanged up to the knee, then a soft roll-off, so midtones keep their tone

static const float kHybridKnee = 0.5;

float3 HdrToProxy(float3 c, uint mode) {
  c = max(c, 0.0);
  if (mode == 1) {
    return c / (1.0 + c);
  }
  if (mode == 2) {
    const float k = kHybridKnee;
    const float3 above = k + (1.0 - k) * (1.0 - exp(-(c - k) / (1.0 - k)));
    return c <= k ? c : above;
  }
  return saturate(c);
}

float3 ProxyToHdr(float3 y, uint mode) {
  y = clamp(y, 0.0, 0.998);
  if (mode == 1) {
    return y / (1.0 - y);
  }
  if (mode == 2) {
    const float k = kHybridKnee;
    const float3 above = k - (1.0 - k) * log(1.0 - (y - k) / (1.0 - k));
    return y <= k ? y : above;
  }
  return y;
}

float3 SrgbToLinear(float3 c) {
  c = saturate(c);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToSrgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// The chain's colour as linear light in the model's range: an HDR chain through the curve, an SDR
// chain (already sRGB-encoded) decoded.
float3 ChainToProxyLinear(float3 chain, uint hdr_chain, uint mode) {
  return hdr_chain ? HdrToProxy(chain, mode) : SrgbToLinear(chain);
}

float3 ProxyLinearToChain(float3 proxy, uint hdr_chain, uint mode) {
  return hdr_chain ? ProxyToHdr(proxy, mode) : LinearToSrgb(proxy);
}

// Bilinear read of a texture of `size` at normalised position `uv`, by four loads (the passes
// have no samplers).
float4 LoadBilinear(Texture2D<float4> source, uint2 size, float2 uv) {
  const float2 position = uv * float2(size) - 0.5;
  const float2 base = floor(position);
  const float2 f = position - base;
  const int2 max_xy = int2(size) - 1;
  const int2 p0 = clamp(int2(base), int2(0, 0), max_xy);
  const int2 p1 = clamp(int2(base) + int2(1, 1), int2(0, 0), max_xy);
  const float4 a = source.Load(int3(p0.x, p0.y, 0));
  const float4 b = source.Load(int3(p1.x, p0.y, 0));
  const float4 c = source.Load(int3(p0.x, p1.y, 0));
  const float4 d = source.Load(int3(p1.x, p1.y, 0));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
