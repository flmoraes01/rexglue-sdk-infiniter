/**
 * @file        gpu/xenos/d3d12/texture_replacement.cpp
 * @brief       Texture dumping and replacement by content hash (texture packs).
 */

#include "gpu/xenos/d3d12/texture_replacement.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

#include <rex/logging/log_category.h>

#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/deferred_command_list.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

namespace {

constexpr u32 kDdsMagic = 0x20534444;  // "DDS "
constexpr u32 kDdpfFourCC = 0x4;
constexpr u32 kDdpfRgb = 0x40;

u32 FourCC(char a, char b, char c, char d) {
  return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
  u32 size, flags, four_cc, rgb_bit_count, r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
  u32 size, flags, height, width, pitch_or_linear_size, depth, mip_map_count, reserved1[11];
  DdsPixelFormat pixel_format;
  u32 caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
  u32 dxgi_format, resource_dimension, misc_flag, array_size, misc_flags2;
};
#pragma pack(pop)

/** Bytes per 4x4 block for block-compressed formats, or 0. */
u32 BlockBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 16;
    default:
      return 0;
  }
}

/** Bytes per pixel for the uncompressed formats packs may use, or 0. */
u32 PixelBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
      return 4;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_R16_UNORM:
      return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
      return 1;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 8;
    default:
      return 0;
  }
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS:
      return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:
      return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:
      return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS:
      return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS:
      return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS:
      return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS:
      return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
      return format;
  }
}

u64 LevelRowBytes(DXGI_FORMAT format, u32 width) {
  if (u32 block = BlockBytes(format)) {
    return u64(std::max(1u, (width + 3) / 4)) * block;
  }
  return u64(width) * PixelBytes(format);
}

u32 LevelRows(DXGI_FORMAT format, u32 height) {
  return BlockBytes(format) ? std::max(1u, (height + 3) / 4) : height;
}

/** The DXGI format a DDS file holds, from its DX10 header or legacy pixel format. */
DXGI_FORMAT DdsFormat(const DdsHeader& header, const DdsHeaderDx10* dx10) {
  if (dx10) {
    return static_cast<DXGI_FORMAT>(dx10->dxgi_format);
  }
  const DdsPixelFormat& pf = header.pixel_format;
  if (pf.flags & kDdpfFourCC) {
    if (pf.four_cc == FourCC('D', 'X', 'T', '1')) return DXGI_FORMAT_BC1_UNORM;
    if (pf.four_cc == FourCC('D', 'X', 'T', '3')) return DXGI_FORMAT_BC2_UNORM;
    if (pf.four_cc == FourCC('D', 'X', 'T', '5')) return DXGI_FORMAT_BC3_UNORM;
    if (pf.four_cc == FourCC('A', 'T', 'I', '1') || pf.four_cc == FourCC('B', 'C', '4', 'U'))
      return DXGI_FORMAT_BC4_UNORM;
    if (pf.four_cc == FourCC('A', 'T', 'I', '2') || pf.four_cc == FourCC('B', 'C', '5', 'U'))
      return DXGI_FORMAT_BC5_UNORM;
    return DXGI_FORMAT_UNKNOWN;
  }
  if ((pf.flags & kDdpfRgb) && pf.rgb_bit_count == 32) {
    if (pf.r_mask == 0x000000FF && pf.b_mask == 0x00FF0000) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (pf.r_mask == 0x00FF0000 && pf.b_mask == 0x000000FF) {
      return pf.a_mask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
    }
  }
  return DXGI_FORMAT_UNKNOWN;
}

const char* FormatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM: return "BC1";
    case DXGI_FORMAT_BC2_UNORM: return "BC2";
    case DXGI_FORMAT_BC3_UNORM: return "BC3";
    case DXGI_FORMAT_BC4_UNORM: return "BC4";
    case DXGI_FORMAT_BC5_UNORM: return "BC5";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    default: return "FMT";
  }
}

// RGBA8 (R in the low byte) from a 5:6:5 colour.
u32 Rgb565(u16 c) {
  const u32 r = ((c >> 11) & 31) * 255 / 31;
  const u32 g = ((c >> 5) & 63) * 255 / 63;
  const u32 b = (c & 31) * 255 / 31;
  return r | (g << 8) | (b << 16);
}

// Colour part of a BC1/BC2/BC3 block into 16 RGBA values.
void DecodeColorBlock(const u8* block, bool bc1, u32 out[16]) {
  u16 c0, c1;
  u32 bits;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  std::memcpy(&bits, block + 4, 4);
  const u32 p0 = Rgb565(c0), p1 = Rgb565(c1);
  u32 palette[4] = {p0 | 0xFF000000u, p1 | 0xFF000000u, 0, 0};
  auto mix = [](u32 a, u32 b, u32 wa, u32 wb, u32 d) {
    u32 r = 0;
    for (u32 shift = 0; shift < 24; shift += 8) {
      r |= ((((a >> shift) & 0xFF) * wa + ((b >> shift) & 0xFF) * wb) / d) << shift;
    }
    return r | 0xFF000000u;
  };
  if (!bc1 || c0 > c1) {
    palette[2] = mix(p0, p1, 2, 1, 3);
    palette[3] = mix(p0, p1, 1, 2, 3);
  } else {
    palette[2] = mix(p0, p1, 1, 1, 2);
    palette[3] = 0;  // transparent black
  }
  for (u32 i = 0; i < 16; ++i) {
    out[i] = palette[(bits >> (2 * i)) & 3];
  }
}

/**
 * Decodes the base level of a read-back texture to RGBA8 (R in the low byte, the channels in the
 * texture's own order). Returns false for a format composites do not support.
 */
bool DecodeToRgba8(DXGI_FORMAT format, u32 width, u32 height, const u8* data, u64 row_pitch,
                   std::vector<u32>& out) {
  out.assign(size_t(width) * height, 0);
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(&out[size_t(y) * width], data + y * row_pitch, size_t(width) * 4);
      }
      return true;
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
      break;
    default:
      return false;
  }
  const bool bc1 = format == DXGI_FORMAT_BC1_UNORM;
  const u32 block_bytes = bc1 ? 8 : 16;
  for (u32 by = 0; by < (height + 3) / 4; ++by) {
    for (u32 bx = 0; bx < (width + 3) / 4; ++bx) {
      const u8* block = data + by * row_pitch + bx * block_bytes;
      u32 texels[16];
      DecodeColorBlock(bc1 ? block : block + 8, bc1, texels);
      if (format == DXGI_FORMAT_BC2_UNORM) {
        u64 alpha;
        std::memcpy(&alpha, block, 8);
        for (u32 i = 0; i < 16; ++i) {
          texels[i] = (texels[i] & 0xFFFFFFu) | ((u32((alpha >> (4 * i)) & 15) * 17) << 24);
        }
      } else if (format == DXGI_FORMAT_BC3_UNORM) {
        const u32 a0 = block[0], a1 = block[1];
        u64 bits = 0;
        std::memcpy(&bits, block + 2, 6);
        u32 alpha[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
        if (a0 > a1) {
          for (u32 i = 1; i < 7; ++i) alpha[1 + i] = ((7 - i) * a0 + i * a1) / 7;
        } else {
          for (u32 i = 1; i < 5; ++i) alpha[1 + i] = ((5 - i) * a0 + i * a1) / 5;
          alpha[6] = 0;
          alpha[7] = 255;
        }
        for (u32 i = 0; i < 16; ++i) {
          texels[i] = (texels[i] & 0xFFFFFFu) | (alpha[(bits >> (3 * i)) & 7] << 24);
        }
      }
      for (u32 i = 0; i < 16; ++i) {
        const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < width && y < height) {
          out[size_t(y) * width + x] = texels[i];
        }
      }
    }
  }
  return true;
}

/** The hash in a pack file name: its last 16 hex digits. */
bool HashFromName(const std::filesystem::path& path, u64& hash) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  const std::string stem = path.stem().string();
  if (ext != ".dds" || stem.size() < 16) {
    return false;
  }
  const std::string hex = stem.substr(stem.size() - 16);
  char* end = nullptr;
  hash = std::strtoull(hex.c_str(), &end, 16);
  return end == hex.c_str() + 16;
}

}  // namespace

void TextureReplacement::Configure(bool dump, bool replace, const std::string& pack_path,
                                   const std::string& overlay_set) {
  dump_ = dump;
  replace_ = replace;
  if (overlay_set != overlay_set_) {
    overlay_set_ = overlay_set;
    ++overlay_generation_;
  }
  if (pack_path == pack_path_setting_) {
    return;
  }
  pack_path_setting_ = pack_path;
  std::filesystem::path root;
  if (pack_path.empty()) {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    root = std::filesystem::path(exe_path).parent_path() / "textures";
  } else {
    root = std::filesystem::path(pack_path);
  }
  dump_dir_ = root / "dump";
  replace_dir_ = root / "replace";
  overlay_dir_ = root / "overlays";
  scanned_ = false;
  overlays_scanned_ = false;
  files_.clear();
  failed_.clear();
  overlay_files_.clear();
  overlay_hashes_.clear();
  ++overlay_generation_;
}

void TextureReplacement::ScanOverlays() {
  overlays_scanned_ = true;
  std::error_code ec;
  if (!std::filesystem::is_directory(overlay_dir_, ec)) {
    return;
  }
  size_t count = 0;
  for (auto set = std::filesystem::directory_iterator(overlay_dir_, ec);
       !ec && set != std::filesystem::directory_iterator(); set.increment(ec)) {
    if (!set->is_directory(ec)) {
      continue;
    }
    const std::string name = set->path().filename().string();
    for (auto it = std::filesystem::directory_iterator(set->path(), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      u64 hash;
      if (it->is_regular_file(ec) && HashFromName(it->path(), hash)) {
        overlay_files_[name][hash] = it->path();
        overlay_hashes_.insert(hash);
        ++count;
      }
    }
  }
  REXGPU_INFO("Texture pack: {} overlay images in {} sets in {}", count, overlay_files_.size(),
              overlay_dir_.string());
}

bool TextureReplacement::HasAnyOverlay(u64 hash) {
  if (!overlays_scanned_) {
    ScanOverlays();
  }
  return overlay_hashes_.contains(hash);
}

const TextureReplacement::Replacement* TextureReplacement::FindComposite(
    u64 hash, ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
  if (overlay_set_.empty() || !HasAnyOverlay(hash)) {
    return nullptr;
  }
  const auto set = overlay_files_.find(overlay_set_);
  if (set == overlay_files_.end() || !set->second.contains(hash)) {
    return nullptr;
  }
  const auto key = std::make_pair(overlay_set_, hash);
  if (auto it = composites_.find(key); it != composites_.end()) {
    return &it->second;
  }
  if (composites_requested_.insert(key).second) {
    PendingDump pending;
    if (ReadBack(resource, state, pending)) {
      pending.overlay_set = overlay_set_;
      pending.hash = hash;
      pending_dumps_.push_back(std::move(pending));
    }
  }
  return nullptr;
}

void TextureReplacement::FinishComposite(const PendingDump& pending, const u8* mapped) {
  std::vector<u32> pixels;
  if (!DecodeToRgba8(pending.format, pending.width, pending.height,
                     mapped + pending.footprint.Offset, pending.footprint.Footprint.RowPitch,
                     pixels)) {
    REXGPU_WARN("Texture pack: overlay for {:016X}: the texture's format {} is not supported",
                pending.hash, FormatName(pending.format));
    return;
  }
  const std::filesystem::path path = overlay_files_[pending.overlay_set][pending.hash];
  DdsImage overlay;
  if (!ReadDds(path, overlay) || overlay.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      overlay.width != pending.width || overlay.height != pending.height ||
      overlay.data.size() - overlay.offset < size_t(overlay.width) * overlay.height * 4) {
    REXGPU_WARN("Texture pack: overlay {} must be R8G8B8A8 and {}x{}", path.filename().string(),
                pending.width, pending.height);
    return;
  }
  const u8* src = overlay.data.data() + overlay.offset;
  // B8G8R8A8 textures keep their order, so the overlay's red and blue swap to match.
  const bool bgra = pending.format == DXGI_FORMAT_B8G8R8A8_UNORM;
  for (size_t i = 0; i < pixels.size(); ++i) {
    u32 value;
    std::memcpy(&value, src + i * 4, 4);
    if (value == 0xFFFF00FFu) {
      continue;  // magenta: keep the texture's pixel
    }
    if (bgra) {
      value = (value & 0xFF00FF00u) | ((value & 0xFF) << 16) | ((value >> 16) & 0xFF);
    }
    pixels[i] = value;
  }
  Replacement composite;
  if (!Upload(bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, pending.width,
              pending.height, 1, reinterpret_cast<const u8*>(pixels.data()), pixels.size() * 4,
              composite)) {
    return;
  }
  REXGPU_INFO("Texture pack: overlay {} drawn over {:016X} ({})", path.filename().string(),
              pending.hash, pending.overlay_set);
  composites_[{pending.overlay_set, pending.hash}] = std::move(composite);
  ++overlay_generation_;
}

void TextureReplacement::ScanReplacements() {
  scanned_ = true;
  std::error_code ec;
  if (!std::filesystem::is_directory(replace_dir_, ec)) {
    return;
  }
  for (auto it = std::filesystem::recursive_directory_iterator(replace_dir_, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::filesystem::path& path = it->path();
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".dds") {
      continue;
    }
    // The hash is the last 16 hex digits of the name.
    const std::string stem = path.stem().string();
    if (stem.size() < 16) {
      continue;
    }
    const std::string hex = stem.substr(stem.size() - 16);
    char* end = nullptr;
    const u64 hash = std::strtoull(hex.c_str(), &end, 16);
    if (end != hex.c_str() + 16) {
      continue;
    }
    files_[hash] = path;
  }
  REXGPU_INFO("Texture pack: {} replacement textures in {}", files_.size(),
              replace_dir_.string());
}

bool TextureReplacement::ReadDds(const std::filesystem::path& path, DdsImage& out) {
  std::ifstream file(path, std::ios::binary);
  out.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  const std::vector<u8>& data = out.data;
  if (data.size() < 4 + sizeof(DdsHeader)) {
    return false;
  }
  u32 magic;
  std::memcpy(&magic, data.data(), 4);
  DdsHeader header;
  std::memcpy(&header, data.data() + 4, sizeof(header));
  if (magic != kDdsMagic || header.size != sizeof(DdsHeader)) {
    return false;
  }
  size_t offset = 4 + sizeof(DdsHeader);
  DdsHeaderDx10 dx10;
  const bool has_dx10 = (header.pixel_format.flags & kDdpfFourCC) &&
                        header.pixel_format.four_cc == FourCC('D', 'X', '1', '0');
  if (has_dx10) {
    if (data.size() < offset + sizeof(dx10)) {
      return false;
    }
    std::memcpy(&dx10, data.data() + offset, sizeof(dx10));
    offset += sizeof(dx10);
  }
  out.offset = offset;
  out.format = TypedFormat(DdsFormat(header, has_dx10 ? &dx10 : nullptr));
  out.width = header.width;
  out.height = header.height;
  out.mips = std::max(1u, header.mip_map_count);
  return out.width && out.height && out.width <= 16384 && out.height <= 16384;
}

bool TextureReplacement::Load(const std::filesystem::path& path, Replacement& out) {
  DdsImage image;
  if (!ReadDds(path, image)) {
    return false;
  }
  if (!BlockBytes(image.format) && !PixelBytes(image.format)) {
    REXGPU_WARN("Texture pack: {} uses a format the pack loader does not read",
                path.filename().string());
    return false;
  }
  return Upload(image.format, image.width, image.height, image.mips,
                image.data.data() + image.offset, image.data.size() - image.offset, out);
}

bool TextureReplacement::Upload(DXGI_FORMAT format, u32 width, u32 height, u32 mips,
                                const u8* data, size_t size, Replacement& out) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(mips);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  Microsoft::WRL::ComPtr<ID3D12Resource> texture;
  if (FAILED(device->CreateCommittedResource(&objects::kHeapPropertiesDefault,
                                             D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&texture)))) {
    return false;
  }
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
  std::vector<UINT> rows(mips);
  std::vector<UINT64> row_sizes(mips);
  UINT64 upload_size = 0;
  device->GetCopyableFootprints(&desc, 0, mips, 0, footprints.data(), rows.data(),
                                row_sizes.data(), &upload_size);
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = upload_size;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> upload;
  if (FAILED(device->CreateCommittedResource(&objects::kHeapPropertiesUpload,
                                             D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload)))) {
    return false;
  }
  u8* mapped = nullptr;
  if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    return false;
  }
  u32 loaded_mips = 0;
  size_t offset = 0;
  for (u32 mip = 0; mip < mips; ++mip) {
    const u32 mip_width = std::max(1u, width >> mip);
    const u32 mip_height = std::max(1u, height >> mip);
    const u64 row_bytes = LevelRowBytes(format, mip_width);
    const u32 mip_rows = LevelRows(format, mip_height);
    if (offset + row_bytes * mip_rows > size) {
      break;  // a truncated file: keep the levels it has
    }
    for (u32 row = 0; row < mip_rows; ++row) {
      std::memcpy(mapped + footprints[mip].Offset + u64(row) * footprints[mip].Footprint.RowPitch,
                  data + offset + row * row_bytes, row_bytes);
    }
    offset += row_bytes * mip_rows;
    ++loaded_mips;
  }
  upload->Unmap(0, nullptr);
  if (!loaded_mips) {
    return false;
  }
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  command_processor_.SubmitBarriers();
  for (u32 mip = 0; mip < loaded_mips; ++mip) {
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprints[mip];
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = texture.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.SubresourceIndex = mip;
    command_list.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  }
  command_processor_.PushTransitionBarrier(
      texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  pending_uploads_.push_back({upload, command_processor_.GetCurrentSubmission()});
  out.resource = texture;
  out.format = format;
  // Without a full chain the view must not reach past the loaded levels.
  out.mip_levels = loaded_mips;
  return true;
}

const TextureReplacement::Replacement* TextureReplacement::Find(u64 hash) {
  if (!replace_) {
    return nullptr;
  }
  if (auto it = loaded_.find(hash); it != loaded_.end()) {
    return &it->second;
  }
  if (!scanned_) {
    ScanReplacements();
  }
  auto file = files_.find(hash);
  if (file == files_.end() || failed_.contains(hash)) {
    return nullptr;
  }
  Replacement replacement;
  if (!Load(file->second, replacement)) {
    REXGPU_WARN("Texture pack: cannot load {}", file->second.filename().string());
    failed_.insert(hash);
    return nullptr;
  }
  REXGPU_INFO("Texture pack: {} replaces {:016X} ({}x{}, {} mips)",
              file->second.filename().string(), hash,
              replacement.resource->GetDesc().Width, replacement.resource->GetDesc().Height,
              replacement.mip_levels);
  return &loaded_.emplace(hash, std::move(replacement)).first->second;
}

void TextureReplacement::Dump(u64 hash, ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                              u32 width, u32 height) {
  if (!dump_ || dumped_.contains(hash)) {
    return;
  }
  dumped_.insert(hash);
  PendingDump pending;
  if (!ReadBack(resource, state, pending)) {
    return;
  }
  pending.path = dump_dir_ / std::format("{}x{}_{}_{:016X}.dds", width, height,
                                         FormatName(pending.format), hash);
  pending_dumps_.push_back(std::move(pending));
}

bool TextureReplacement::ReadBack(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                                  PendingDump& pending) {
  D3D12_RESOURCE_DESC desc = resource->GetDesc();
  const DXGI_FORMAT format = TypedFormat(desc.Format);
  if (!BlockBytes(format) && !PixelBytes(format)) {
    return false;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 size = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &pending.footprint, &rows, &row_bytes, &size);
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = size;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&objects::kHeapPropertiesReadback,
                                             D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&pending.buffer)))) {
    return false;
  }
  command_processor_.PushTransitionBarrier(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = resource;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dest = {};
  dest.pResource = pending.buffer.Get();
  dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dest.PlacedFootprint = pending.footprint;
  command_processor_.GetDeferredCommandList().D3DCopyTextureRegion(&dest, 0, 0, 0, &source,
                                                                   nullptr);
  command_processor_.PushTransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  pending.rows = rows;
  pending.row_bytes = row_bytes;
  pending.width = static_cast<u32>(desc.Width);
  pending.height = desc.Height;
  pending.format = format;
  pending.submission = command_processor_.GetCurrentSubmission();
  return true;
}

void TextureReplacement::Poll() {
  const u64 completed = command_processor_.GetCompletedSubmission();
  std::erase_if(pending_uploads_,
                [completed](const PendingUpload& upload) { return upload.submission <= completed; });
  for (auto it = pending_dumps_.begin(); it != pending_dumps_.end();) {
    if (it->submission > completed) {
      ++it;
      continue;
    }
    u8* mapped = nullptr;
    if (it->path.empty()) {
      if (SUCCEEDED(it->buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
        FinishComposite(*it, mapped);
        it->buffer->Unmap(0, nullptr);
      }
    } else if (SUCCEEDED(it->buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
      std::error_code ec;
      std::filesystem::create_directories(dump_dir_, ec);
      DdsHeader header = {};
      header.size = sizeof(DdsHeader);
      header.flags = 0x1 | 0x2 | 0x4 | 0x1000;  // caps, height, width, pixel format
      header.width = it->width;
      header.height = it->height;
      header.mip_map_count = 1;
      header.pixel_format.size = sizeof(DdsPixelFormat);
      header.pixel_format.flags = kDdpfFourCC;
      header.pixel_format.four_cc = FourCC('D', 'X', '1', '0');
      header.caps = 0x1000;  // texture
      DdsHeaderDx10 dx10 = {};
      dx10.dxgi_format = it->format;
      dx10.resource_dimension = 3;  // texture 2D
      dx10.array_size = 1;
      std::ofstream file(it->path, std::ios::binary);
      file.write(reinterpret_cast<const char*>(&kDdsMagic), 4);
      file.write(reinterpret_cast<const char*>(&header), sizeof(header));
      file.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));
      for (u32 row = 0; row < it->rows; ++row) {
        file.write(reinterpret_cast<const char*>(mapped + it->footprint.Offset +
                                                 u64(row) * it->footprint.Footprint.RowPitch),
                   static_cast<std::streamsize>(it->row_bytes));
      }
      it->buffer->Unmap(0, nullptr);
    }
    it = pending_dumps_.erase(it);
  }
}

}  // namespace rex::gpu::d3d12
