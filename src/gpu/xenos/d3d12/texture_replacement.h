#pragma once

/**
 * @file        gpu/xenos/d3d12/texture_replacement.h
 * @brief       Texture dumping and replacement by content hash (texture packs).
 *
 * A guest texture's base level is identified by an XXH3 hash of its guest data. Dumping writes the
 * texture as loaded on the host to <pack>/dump/<W>x<H>_<format>_<hash>.dds; replacing looks for
 * any file named *_<hash>.dds (or <hash>.dds) under <pack>/replace and shows that instead, at
 * whatever size and with whatever mips it has. The file must be in the host's channel layout,
 * which dumped files already are.
 *
 * Overlays draw over a texture instead of replacing all of it: <pack>/overlays/<set>/<name>_<hash>.dds
 * is an R8G8B8A8 image of the texture's size where magenta (255, 0, 255, 255) keeps the guest
 * pixel and anything else, transparent included, replaces it. The composite is made from the
 * loaded texture read back from the GPU, so a project ships only its own pixels (button prompts).
 */

#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/types.h>

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class TextureReplacement {
 public:
  struct Replacement {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 mip_levels = 1;
  };

  explicit TextureReplacement(D3D12CommandProcessor& command_processor)
      : command_processor_(command_processor) {}

  /**
   * Reads the settings; call once per frame or load, cheap when nothing changed.
   *
   * @param overlay_set The subfolder of <pack>/overlays to draw, empty for none.
   */
  void Configure(bool dump, bool replace, const std::string& pack_path,
                 const std::string& overlay_set);
  bool IsDumping() const { return dump_; }
  bool IsReplacing() const { return replace_; }

  /** The replacement for a hash, loaded and uploaded on first use; null when there is none. */
  const Replacement* Find(u64 hash);

  /** Copies subresource 0 of a texture out and writes it as a DDS once the GPU is done. */
  void Dump(u64 hash, ID3D12Resource* resource, D3D12_RESOURCE_STATES state, u32 width,
            u32 height);

  /** True when an overlay set is chosen. */
  bool IsOverlaying() const { return !overlay_set_.empty(); }
  /** True when any overlay set has an image for the hash. */
  bool HasAnyOverlay(u64 hash);
  /**
   * The texture with the current set's overlay drawn over it. Null when the set has no image for
   * the hash, or while the composite is being made from `resource` (requested on the first call;
   * GetOverlayGeneration changes when it is ready).
   */
  const Replacement* FindComposite(u64 hash, ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES state);
  /** Changes when the overlay set changes or a composite finishes. */
  u32 GetOverlayGeneration() const { return overlay_generation_; }

  /** Writes finished dumps, makes finished composites and frees finished uploads. */
  void Poll();

 private:
  struct DdsImage {
    std::vector<u8> data;
    size_t offset = 0;  // of the first level
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 width = 0, height = 0, mips = 1;
  };
  void ScanReplacements();
  void ScanOverlays();
  bool ReadDds(const std::filesystem::path& path, DdsImage& out);
  bool Load(const std::filesystem::path& path, Replacement& out);
  bool Upload(DXGI_FORMAT format, u32 width, u32 height, u32 mips, const u8* data, size_t size,
              Replacement& out);

  D3D12CommandProcessor& command_processor_;
  bool dump_ = false;
  bool replace_ = false;
  std::string pack_path_setting_ = "\x01";
  std::filesystem::path dump_dir_;
  std::filesystem::path replace_dir_;
  std::filesystem::path overlay_dir_;
  std::string overlay_set_;
  u32 overlay_generation_ = 0;
  bool overlays_scanned_ = false;
  // Set name, then hash.
  std::map<std::string, std::unordered_map<u64, std::filesystem::path>> overlay_files_;
  std::unordered_set<u64> overlay_hashes_;
  std::map<std::pair<std::string, u64>, Replacement> composites_;
  std::set<std::pair<std::string, u64>> composites_requested_;
  bool scanned_ = false;
  std::unordered_map<u64, std::filesystem::path> files_;
  std::unordered_map<u64, Replacement> loaded_;
  std::unordered_set<u64> failed_;
  std::unordered_set<u64> dumped_;
  struct PendingDump {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    u32 rows;
    u64 row_bytes;
    u32 width, height;
    DXGI_FORMAT format;
    std::filesystem::path path;  // a dump file, or empty for a composite
    std::string overlay_set;     // composites: the set and hash it is for
    u64 hash = 0;
    u64 submission;
  };
  bool ReadBack(ID3D12Resource* resource, D3D12_RESOURCE_STATES state, PendingDump& out);
  void FinishComposite(const PendingDump& pending, const u8* mapped);
  std::vector<PendingDump> pending_dumps_;
  struct PendingUpload {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    u64 submission;
  };
  std::vector<PendingUpload> pending_uploads_;
};

}  // namespace rex::gpu::d3d12
