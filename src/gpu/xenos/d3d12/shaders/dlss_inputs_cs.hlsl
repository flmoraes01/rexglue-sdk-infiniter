// Builds the inputs DLSS needs from the guest scene: a float copy of the depth buffer and
// camera motion vectors. Motion is reprojected from depth with a matrix that maps this frame's
// clip space to the previous frame's, so it covers camera movement only.
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssInputsCS
//            /Fh bytecode/d3d12_5_1/dlss_inputs_cs.h dlss_inputs_cs.hlsl

cbuffer constants : register(b0) {
  // Rows of the current clip -> previous clip matrix (row vector: prev = v * M).
  float4 xe_reproject_row0;
  float4 xe_reproject_row1;
  float4 xe_reproject_row2;
  float4 xe_reproject_row3;
  uint2 xe_depth_origin;  // where the scene starts in the depth render target, in pixels
  uint2 xe_size;          // scene size in pixels
};

Texture2D<float> xe_depth : register(t0);
RWTexture2D<float> xe_depth_out : register(u0);
RWTexture2D<float2> xe_motion_out : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  float depth = xe_depth.Load(int3(xe_depth_origin + id.xy, 0));
  xe_depth_out[id.xy] = depth;

  float2 uv = (float2(id.xy) + 0.5) / float2(xe_size);
  float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  float4 previous = clip.x * xe_reproject_row0 + clip.y * xe_reproject_row1 +
                    clip.z * xe_reproject_row2 + clip.w * xe_reproject_row3;
  float2 motion = 0.0;
  if (previous.w > 1.0e-6) {
    float2 previous_ndc = previous.xy / previous.w;
    float2 previous_uv = float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
    // DLSS wants motion from the current pixel to where it was, in pixels.
    motion = (previous_uv - uv) * float2(xe_size);
  }
  xe_motion_out[id.xy] = motion;
}
