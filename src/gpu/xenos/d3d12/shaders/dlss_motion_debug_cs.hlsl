// Debug view of the motion vectors (gpu.d3d12.dlss_debug_view = motion), in place of the scene:
// red and green are the horizontal and vertical motion DLSS gets (full at 16 pixels), blue where it
// is the scene draws' own (object motion) rather than the camera's. It also sums, for the log, the
// object motion and the camera motion over the pixels with object motion, and the camera motion
// elsewhere (the title's post-processing makes the view itself hard to read).
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssMotionDebugCS
//            /Fh bytecode/d3d12_5_1/dlss_motion_debug_cs.h dlss_motion_debug_cs.hlsl

cbuffer constants : register(b0) {
  // The same as dlss_inputs_cs.
  float4 xe_reproject_row0;
  float4 xe_reproject_row1;
  float4 xe_reproject_row2;
  float4 xe_reproject_row3;
  uint2 xe_depth_origin;
  uint2 xe_size;
  uint xe_object_motion_valid;
};

Texture2D<float> xe_depth : register(t0);
Texture2D<float2> xe_object_motion : register(t1);
Texture2D<float> xe_reactive : register(t2);
RWTexture2D<float4> xe_dest : register(u0);
// Object pixels, object motion and camera motion summed there (in quarter pixels, each capped at
// 64 pixels), other pixels, camera motion summed there, and the difference between the object and
// the camera motion summed over the object pixels, and the reactive pixels.
RWByteAddressBuffer xe_stats : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  float depth = xe_depth.Load(int3(xe_depth_origin + id.xy, 0));
  float2 uv = (float2(id.xy) + 0.5) / float2(xe_size);
  float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  float4 previous = clip.x * xe_reproject_row0 + clip.y * xe_reproject_row1 +
                    clip.z * xe_reproject_row2 + clip.w * xe_reproject_row3;
  float2 camera_motion = 0.0;
  if (previous.w > 1.0e-6) {
    float2 previous_ndc = previous.xy / previous.w;
    float2 previous_uv = float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
    camera_motion = (previous_uv - uv) * float2(xe_size);
  }
  float2 motion = camera_motion;
  bool is_object = false;
  if (xe_object_motion_valid) {
    float2 object_motion = xe_object_motion.Load(int3(xe_depth_origin + id.xy, 0));
    if (all((asuint(object_motion) & 0x7FFFFFFFu) < 0x7F800000u)) {
      motion = object_motion;
      is_object = true;
    }
  }
  if (xe_reactive.Load(int3(xe_depth_origin + id.xy, 0)) > 0.5) {
    xe_stats.InterlockedAdd(24, 1u);
  }
  uint camera_length = uint(min(length(camera_motion), 64.0) * 4.0);
  if (is_object) {
    xe_stats.InterlockedAdd(0, 1u);
    xe_stats.InterlockedAdd(4, uint(min(length(motion), 64.0) * 4.0));
    xe_stats.InterlockedAdd(8, camera_length);
    xe_stats.InterlockedAdd(20, uint(min(length(motion - camera_motion), 64.0) * 4.0));
  } else {
    xe_stats.InterlockedAdd(12, 1u);
    xe_stats.InterlockedAdd(16, camera_length);
  }
  xe_dest[id.xy] =
      float4(saturate(abs(motion) * (1.0 / 16.0)), is_object ? 0.5 : 0.0, 1.0);
}
