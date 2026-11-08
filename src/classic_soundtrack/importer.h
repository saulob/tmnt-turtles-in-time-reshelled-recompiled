#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "classic_soundtrack/known_roms.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack/soundset.h"

namespace tmnt::classic {

inline constexpr std::uint32_t kImporterVersion = 3;
inline constexpr std::uint32_t kOldestCompatibleImporter = 2;
inline constexpr char kSoundtrackFolder[] = "snes_soundtrack";
inline constexpr char kSoundSetFile[] = "soundset.bin";
inline constexpr char kManifestFile[] = "manifest.toml";

enum class ImportFailure {
  kNone,
  kInvalidRom,
  kUnsupportedRom,
  kUnsupportedSoundtrack,
  kCorruptSoundtrack,
  kImportFailed,
};

const char* ImportFailureText(ImportFailure failure);

struct RomAnalysis {
  ImportFailure failure = ImportFailure::kInvalidRom;
  std::string detail;
  ClassicSoundtrackManifest manifest;
  SoundSet set;
  std::vector<std::uint8_t> soundset_bytes;
  bool copier_header = false;
  std::string title;
  std::string rom_sha256;
  std::string fingerprint;
  // SNES Gameplay data from the same ROM; empty when its gameplay code is not a known one.
  std::vector<std::uint8_t> gameplay_bytes;
  std::string gameplay_detail;
};

// Read-only: validates the ROM and derives the sound set without touching the disk.
RomAnalysis AnalyzeRom(std::span<const std::uint8_t> file,
                       std::string_view expected_fingerprint = kKnownSoundtrackFingerprint);

struct ImportFileOps {
  std::function<void(const std::filesystem::path&, const std::filesystem::path&, std::error_code&)> rename =
      [](const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
        std::filesystem::rename(from, to, ec);
      };
  std::function<void(const std::filesystem::path&, std::error_code&)> remove_all =
      [](const std::filesystem::path& path, std::error_code& ec) { std::filesystem::remove_all(path, ec); };
};

struct ImportResult {
  bool ok = false;
  ImportFailure failure = ImportFailure::kNone;
  std::string detail;
  ClassicSoundtrackManifest manifest;
  bool gameplay = false;
  std::string gameplay_detail;
};

std::filesystem::path SoundtrackFolder(const std::filesystem::path& user_data_root);

ImportResult ImportSoundtrackFromBytes(std::span<const std::uint8_t> rom_file, const std::filesystem::path& user_data_root,
                                       const ImportFileOps& ops = {},
                                       std::string_view expected_fingerprint = kKnownSoundtrackFingerprint);
// Reads a .sfc or .smc file, or the one ROM inside a .zip.
ImportFailure ReadRomFile(const std::filesystem::path& rom_path, std::vector<std::uint8_t>& rom, std::string& detail);
ImportResult ImportSoundtrack(const std::filesystem::path& rom_path, const std::filesystem::path& user_data_root,
                              const ImportFileOps& ops = {});

enum class SoundtrackStatus { kNotImported, kReady, kUnsupportedRom, kImportFailed };

struct StatusResult {
  SoundtrackStatus status = SoundtrackStatus::kNotImported;
  std::string detail;
  ClassicSoundtrackManifest manifest;
};

const char* SoundtrackStatusText(SoundtrackStatus status);

// When Ready and loaded is not null, also hands back the validated sound set.
StatusResult QuerySoundtrackStatus(const std::filesystem::path& user_data_root, SoundSet* loaded = nullptr);
SoundtrackStatus StatusForImport(const ImportResult& result);

// Deletes only <user_data_root>/snes_soundtrack (the soundtrack and the SNES Gameplay data) and its staging leftovers.
bool RemoveSoundtrack(const std::filesystem::path& user_data_root, std::string& error,
                      const ImportFileOps& ops = {});

}  // namespace tmnt::classic
