// Screen-space global illumination on the 3D scene: horizon-style ambient occlusion and one
// bounce of indirect light gathered from nearby lit pixels, from depth and colour only. Runs
// before the upscaler, whose temporal accumulation removes the per-frame noise; without one the
// noise pattern is fixed per pixel.
//
// Build: fxc /nologo /T cs_5_0 /E main /O3 /Vn kSsgiCS /Fh bytecode/d3d12_5_1/ssgi_cs.h ssgi_cs.hlsl

cbuffer constants : register(b0) {
  uint2 xe_color_origin;   // where the scene starts in the colour texture
  uint2 xe_size;           // scene size in pixels (rendered)
  float xe_near;
  float xe_far;
  float2 xe_tan_half_fov;  // x, y
  float xe_radius;         // gather radius, in view units
  float xe_ao_strength;
  float xe_gi_strength;
  uint xe_frame;           // varies the pattern per frame for temporal accumulation
};

Texture2D<float4> xe_color : register(t0);
Texture2D<float> xe_depth : register(t1);  // non-linear, 0 near, 1 far, rendered size
RWTexture2D<float4> xe_out : register(u0);

static const float kPi = 3.14159265;
static const uint kSlices = 4;
static const uint kSteps = 6;

float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4); }
float3 LinearToSrgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055;
}

float ViewZ(float depth) {
  // Conventional depth: z_ndc = f (z - n) / (z (f - n)).
  return xe_near * xe_far / max(xe_far - depth * (xe_far - xe_near), 1.0e-4);
}

float3 ViewPos(int2 p) {
  const float z = ViewZ(xe_depth.Load(int3(clamp(p, int2(0, 0), int2(xe_size) - 1), 0)));
  const float2 uv = (float2(p) + 0.5) / float2(xe_size);
  const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  return float3(ndc * xe_tan_half_fov * z, z);
}

float InterleavedGradientNoise(float2 p) {
  return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  const int2 p = int2(id.xy);
  const float4 color_encoded = xe_color.Load(int3(int2(xe_color_origin) + p, 0));
  const float depth = xe_depth.Load(int3(p, 0));
  if (depth >= 0.9999) {
    xe_out[id.xy] = color_encoded;  // sky: nothing to occlude or bounce
    return;
  }
  const float3 center = ViewPos(p);
  // Normal from the closer neighbour on each axis, which keeps edges clean.
  const float3 left = ViewPos(p - int2(1, 0)), right = ViewPos(p + int2(1, 0));
  const float3 up = ViewPos(p - int2(0, 1)), down = ViewPos(p + int2(0, 1));
  const float3 dx = abs(right.z - center.z) < abs(center.z - left.z) ? right - center : center - left;
  const float3 dy = abs(down.z - center.z) < abs(center.z - up.z) ? down - center : center - up;
  float3 normal = normalize(cross(dy, dx));
  if (dot(normal, -center) < 0.0) {
    normal = -normal;
  }

  // Radius in pixels at this depth.
  const float pixels_per_unit = float(xe_size.y) * 0.5 / (xe_tan_half_fov.y * center.z);
  const float radius_pixels = clamp(xe_radius * pixels_per_unit, 4.0, float(xe_size.y) * 0.15);
  const float noise = InterleavedGradientNoise(float2(p) + float(xe_frame % 64) * 5.588238);
  const float jitter = InterleavedGradientNoise(float2(p.yx) + float(xe_frame % 64) * 3.1);

  float occlusion = 0.0;
  float3 bounce = 0.0;
  float weight_sum = 0.0;
  [loop] for (uint slice = 0; slice < kSlices; ++slice) {
    const float angle = (float(slice) + noise) * (kPi / float(kSlices)) * 2.0;
    const float2 direction = float2(cos(angle), sin(angle));
    [loop] for (uint step = 0; step < kSteps; ++step) {
      const float t = (float(step) + jitter) / float(kSteps);
      const int2 q = p + int2(direction * radius_pixels * t * t + direction * 1.5);
      if (any(q < 0) || any(q >= int2(xe_size))) {
        break;
      }
      const float3 sample_pos = ViewPos(q);
      const float3 v = sample_pos - center;
      const float distance = length(v);
      const float n_dot_v = dot(normal, v / max(distance, 1.0e-4));
      const float falloff = saturate(1.0 - distance / xe_radius);
      // Occluded by geometry above the surface's tangent plane, nearer than the radius.
      occlusion += saturate(n_dot_v - 0.1) * falloff;
      // Light from that point, as seen from here.
      const float3 sample_color =
          SrgbToLinear(xe_color.Load(int3(int2(xe_color_origin) + q, 0)).rgb);
      bounce += sample_color * saturate(n_dot_v) * falloff;
      weight_sum += 1.0;
    }
  }
  const float ao = weight_sum > 0.0 ? 1.0 - saturate(occlusion / weight_sum * 2.0) : 1.0;
  const float3 indirect = weight_sum > 0.0 ? bounce / weight_sum : 0.0;

  const float3 color = SrgbToLinear(color_encoded.rgb);
  // The surface's own colour tints the bounce it receives (a rough albedo from its brightness).
  const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
  const float3 albedo = color / max(luma, 0.05) * saturate(luma * 4.0);
  float3 result = color * lerp(1.0, ao, xe_ao_strength);
  result += indirect * albedo * xe_gi_strength;
  xe_out[id.xy] = float4(LinearToSrgb(result), color_encoded.a);
}
