// Copies the object motion that the motion vector pixel shaders wrote to the region after EDRAM
// (one dword per depth sample, at the sample's depth address) into the scene motion texture, for
// the part of the scene a depth resolve copies, and clears that region for the next tile.
//
// A dword is the inverted pair of halves (previous minus current NDC); zero means no motion was
// written there, which becomes NaN in the texture (the camera motion is used instead).
//
// Build: fxc /nologo /T cs_5_1 /E main /O3 /Vn kDlssMotionCopyCS
//            /Fh bytecode/d3d12_5_1/dlss_motion_copy_cs.h dlss_motion_copy_cs.hlsl

cbuffer constants : register(b0) {
  uint xe_base_tiles;          // EDRAM tile of the resolved region's origin
  uint xe_pitch_tiles;         // depth buffer row pitch in tiles
  uint2 xe_sample_count_log2;  // samples per pixel in x and y
  uint2 xe_tile_size;          // tile size in samples, with the resolution scale
  uint2 xe_source_origin;      // the region's first pixel relative to its origin tile
  uint2 xe_size;               // region size in pixels
  uint2 xe_dest_origin;        // where the region goes in the motion texture
  float2 xe_ndc_to_pixels;     // half the scene viewport in pixels, y negated
  uint xe_motion_region;       // EDRAM size in dwords, where the motion region starts
};

RWBuffer<uint> xe_edram : register(u0);
RWTexture2D<float2> xe_motion : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_size)) {
    return;
  }
  // The first sample's depth address, as the ROV pixel shaders compute it.
  uint2 sample_xy = (xe_source_origin + id.xy) << xe_sample_count_log2;
  uint2 tile_xy = sample_xy / xe_tile_size;
  uint2 in_tile = sample_xy - tile_xy * xe_tile_size;
  uint half_width = xe_tile_size.x >> 1;
  // Depth swaps the halves of each tile.
  uint x = in_tile.x >= half_width ? in_tile.x - half_width : in_tile.x + half_width;
  uint address = (xe_base_tiles + tile_xy.y * xe_pitch_tiles + tile_xy.x) *
                     (xe_tile_size.x * xe_tile_size.y) +
                 in_tile.y * xe_tile_size.x + x;
  address = address % xe_motion_region + xe_motion_region;

  uint packed = xe_edram[address];
  xe_edram[address] = 0u;
  float2 motion = asfloat(0x7FC00000u).xx;
  if (packed != 0u) {
    packed = ~packed;
    motion = f16tof32(uint2(packed & 0xFFFFu, packed >> 16)) * xe_ndc_to_pixels;
  }
  xe_motion[xe_dest_origin + id.xy] = motion;
}
