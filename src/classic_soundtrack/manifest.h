#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tmnt::classic {

struct ManifestSong {
  std::uint16_t id = 0;
  std::string label;
  std::uint8_t command = 0;
  std::uint16_t stage_pack = 0xFFFF;
};

struct ClassicSoundtrackManifest {
  std::uint32_t format_version = 0;
  std::uint32_t importer_version = 0;
  std::string source_rom_sha256;
  std::string source_region;
  std::string soundtrack_fingerprint;
  std::string soundset_sha256;
  std::uint32_t song_count = 0;
  std::vector<ManifestSong> songs;
};

std::string SerializeManifest(const ClassicSoundtrackManifest& manifest);

// Reads the subset of TOML SerializeManifest writes; false when a required key is missing or malformed.
bool ParseManifest(std::string_view text, ClassicSoundtrackManifest& manifest);

}  // namespace tmnt::classic
