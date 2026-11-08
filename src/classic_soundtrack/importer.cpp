#include "classic_soundtrack/importer.h"

#include <fstream>

#include "classic_soundtrack/known_roms.h"
#include "classic_soundtrack/rom_zip.h"
#include "classic_soundtrack/snes_rom.h"
#include "classic_soundtrack/sound_parser.h"
#include "updater/sha256.h"

namespace tmnt::classic {
namespace fs = std::filesystem;
namespace {

constexpr size_t kMaxRomFile = kRomSize + kCopierHeaderSize;
constexpr size_t kMaxZipFile = 8 * 1024 * 1024;
constexpr size_t kMaxManifestFile = 1024 * 1024;
constexpr size_t kMaxSoundSetFile = 8 * 1024 * 1024;
constexpr char kStagingSuffix[] = ".new";
constexpr char kBackupSuffix[] = ".old";
constexpr char kPartSuffix[] = ".part";

std::string AsText(std::span<const std::uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool ReadFileCapped(const fs::path& path, size_t max_size, std::vector<std::uint8_t>& out) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec || size > max_size) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  out.resize(size_t(size));
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  return in.gcount() == std::streamsize(out.size());
}

fs::path Sibling(const fs::path& root, const char* suffix) {
  return root / (std::string(kSoundtrackFolder) + suffix);
}

bool WriteFileAtomic(const fs::path& path, std::span<const std::uint8_t> bytes, const ImportFileOps& ops) {
  const fs::path part = fs::path(path).concat(kPartSuffix);
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  ops.rename(part, path, ec);
  return !ec;
}

ManifestSong SongEntry(const SoundSet& set, size_t id) {
  ManifestSong entry;
  entry.id = std::uint16_t(id);
  entry.label = std::string(SongLabel(id));
  entry.command = set.songs[id].command;
  entry.stage_pack = set.songs[id].pack_id;
  return entry;
}

ImportFailure FailureFor(RomStatus status) {
  return status == RomStatus::kOk ? ImportFailure::kNone : ImportFailure::kInvalidRom;
}

void RecoverInterruptedInstall(const fs::path& root, const ImportFileOps& ops) {
  std::error_code ec;
  const fs::path folder = SoundtrackFolder(root);
  const fs::path backup = Sibling(root, kBackupSuffix);
  if (!fs::exists(folder, ec) && fs::exists(backup, ec)) {
    ops.rename(backup, folder, ec);
  }
}

// The previous install is moved aside and only deleted once the new folder is in place.
bool InstallFolder(const fs::path& root, std::span<const std::uint8_t> soundset, const std::string& manifest_text,
                   const ImportFileOps& ops, std::string& error) {
  const fs::path folder = SoundtrackFolder(root);
  const fs::path staging = Sibling(root, kStagingSuffix);
  const fs::path backup = Sibling(root, kBackupSuffix);
  std::error_code ec;

  fs::create_directories(root, ec);
  if (ec) {
    error = "cannot create the user data folder";
    return false;
  }
  RecoverInterruptedInstall(root, ops);
  ops.remove_all(staging, ec);
  ops.remove_all(backup, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    error = "cannot create the staging folder";
    return false;
  }

  const auto discard_staging = [&]() {
    std::error_code ignored;
    ops.remove_all(staging, ignored);
  };

  const std::span<const std::uint8_t> manifest_bytes(reinterpret_cast<const std::uint8_t*>(manifest_text.data()),
                                                     manifest_text.size());
  if (!WriteFileAtomic(staging / kSoundSetFile, soundset, ops) ||
      !WriteFileAtomic(staging / kManifestFile, manifest_bytes, ops)) {
    discard_staging();
    error = "cannot write the sound set files";
    return false;
  }

  const bool had_previous = fs::exists(folder, ec);
  if (had_previous) {
    ops.rename(folder, backup, ec);
    if (ec) {
      discard_staging();
      error = "cannot replace the previous import";
      return false;
    }
  }
  ops.rename(staging, folder, ec);
  if (ec) {
    std::error_code restore;
    if (had_previous) {
      ops.rename(backup, folder, restore);
    }
    discard_staging();
    error = "cannot move the new import into place";
    return false;
  }
  if (had_previous) {
    ops.remove_all(backup, ec);
  }
  return true;
}

}  // namespace

const char* ImportFailureText(ImportFailure failure) {
  switch (failure) {
    case ImportFailure::kNone: return "No error";
    case ImportFailure::kInvalidRom: return "Invalid ROM";
    case ImportFailure::kUnsupportedRom: return "Unsupported ROM";
    case ImportFailure::kUnsupportedSoundtrack: return "Unsupported soundtrack";
    case ImportFailure::kCorruptSoundtrack: return "Corrupt soundtrack data";
    case ImportFailure::kImportFailed: return "Import failed";
  }
  return "Import failed";
}

const char* SoundtrackStatusText(SoundtrackStatus status) {
  switch (status) {
    case SoundtrackStatus::kNotImported: return "Not imported";
    case SoundtrackStatus::kReady: return "Ready";
    case SoundtrackStatus::kUnsupportedRom: return "Unsupported ROM";
    case SoundtrackStatus::kImportFailed: return "Import failed";
  }
  return "Import failed";
}

fs::path SoundtrackFolder(const fs::path& user_data_root) {
  return user_data_root / kSoundtrackFolder;
}

RomAnalysis AnalyzeRom(std::span<const std::uint8_t> file, std::string_view expected_fingerprint) {
  RomAnalysis analysis;
  std::span<const std::uint8_t> rom;
  RomStatus status = NormalizeRom(file, rom);
  if (status != RomStatus::kOk) {
    analysis.detail = "the file is not a 1 MiB SNES ROM image";
    return analysis;
  }
  analysis.copier_header = rom.size() != file.size();
  RomHeaderInfo header;
  status = ValidateLoRomHeader(rom, &header);
  if (status != RomStatus::kOk) {
    analysis.failure = FailureFor(status);
    analysis.detail = status == RomStatus::kBadChecksum ? "the SNES header checksum is wrong"
                                                         : "the SNES header is not a 1 MiB LoROM header";
    return analysis;
  }
  analysis.title = header.title;
  analysis.rom_sha256 = trg::update::Sha256Hex(AsText(rom));

  ParseResult parsed = ParseSoundSet(rom);
  if (parsed.category != ParseCategory::kOk) {
    analysis.failure = parsed.category == ParseCategory::kData ? ImportFailure::kCorruptSoundtrack
                                                               : ImportFailure::kUnsupportedRom;
    analysis.detail = std::move(parsed.detail);
    return analysis;
  }

  analysis.fingerprint = SoundtrackFingerprint(parsed.set);
  if (analysis.fingerprint != expected_fingerprint) {
    analysis.failure = ImportFailure::kUnsupportedSoundtrack;
    analysis.detail = "the soundtrack data is not a known one";
    return analysis;
  }

  const std::string_view known_region = KnownRomRegion(analysis.rom_sha256);

  analysis.set = std::move(parsed.set);
  analysis.soundset_bytes = SerializeSoundSet(analysis.set);

  ClassicSoundtrackManifest& manifest = analysis.manifest;
  manifest.format_version = kSoundSetFormatVersion;
  manifest.importer_version = kImporterVersion;
  manifest.source_rom_sha256 = analysis.rom_sha256;
  manifest.source_region = std::string(known_region.empty() ? kCompatibleRegion : known_region);
  manifest.soundtrack_fingerprint = analysis.fingerprint;
  manifest.soundset_sha256 = trg::update::Sha256Hex(AsText(analysis.soundset_bytes));
  manifest.song_count = std::uint32_t(analysis.set.songs.size());
  for (size_t id = 0; id < analysis.set.songs.size(); ++id) {
    manifest.songs.push_back(SongEntry(analysis.set, id));
  }
  analysis.failure = ImportFailure::kNone;
  return analysis;
}

ImportResult ImportSoundtrackFromBytes(std::span<const std::uint8_t> rom_file, const fs::path& user_data_root,
                                       const ImportFileOps& ops, std::string_view expected_fingerprint) {
  ImportResult result;
  RomAnalysis analysis = AnalyzeRom(rom_file, expected_fingerprint);
  if (analysis.failure != ImportFailure::kNone) {
    result.failure = analysis.failure;
    result.detail = std::move(analysis.detail);
    return result;
  }
  std::string error;
  if (!InstallFolder(user_data_root, analysis.soundset_bytes, SerializeManifest(analysis.manifest), ops, error)) {
    result.failure = ImportFailure::kImportFailed;
    result.detail = std::move(error);
    return result;
  }
  result.ok = true;
  result.manifest = std::move(analysis.manifest);
  return result;
}

ImportFailure ReadRomFile(const fs::path& rom_path, std::vector<std::uint8_t>& rom, std::string& detail) {
  const bool zip = IsZipPath(rom_path.extension().string());
  const size_t limit = zip ? kMaxZipFile : kMaxRomFile;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(rom_path, ec);
  if (ec) {
    detail = "cannot read the ROM file";
    return ImportFailure::kImportFailed;
  }
  if (size > limit) {
    detail = "the file is not a 1 MiB SNES ROM image";
    return ImportFailure::kInvalidRom;
  }
  std::vector<std::uint8_t> file;
  if (!ReadFileCapped(rom_path, limit, file)) {
    detail = "cannot read the ROM file";
    return ImportFailure::kImportFailed;
  }
  if (!zip) {
    rom = std::move(file);
    return ImportFailure::kNone;
  }
  return ExtractRomFromZip(file, kMaxRomFile, rom, detail) ? ImportFailure::kNone : ImportFailure::kInvalidRom;
}

ImportResult ImportSoundtrack(const fs::path& rom_path, const fs::path& user_data_root, const ImportFileOps& ops) {
  ImportResult result;
  std::vector<std::uint8_t> rom;
  result.failure = ReadRomFile(rom_path, rom, result.detail);
  if (result.failure != ImportFailure::kNone) {
    return result;
  }
  return ImportSoundtrackFromBytes(rom, user_data_root, ops);
}

StatusResult QuerySoundtrackStatus(const fs::path& user_data_root, SoundSet* loaded) {
  StatusResult status;
  const fs::path folder = SoundtrackFolder(user_data_root);
  std::error_code ec;
  const bool has_manifest = fs::is_regular_file(folder / kManifestFile, ec);
  const bool has_soundset = fs::is_regular_file(folder / kSoundSetFile, ec);
  if (!has_manifest && !has_soundset) {
    return status;
  }
  status.status = SoundtrackStatus::kImportFailed;
  if (!has_manifest || !has_soundset) {
    status.detail = "the imported soundtrack is incomplete";
    return status;
  }

  std::vector<std::uint8_t> manifest_bytes;
  ClassicSoundtrackManifest manifest;
  if (!ReadFileCapped(folder / kManifestFile, kMaxManifestFile, manifest_bytes) ||
      !ParseManifest(AsText(manifest_bytes), manifest)) {
    status.detail = "the manifest cannot be read";
    return status;
  }
  if (manifest.format_version != kSoundSetFormatVersion || manifest.importer_version < kOldestCompatibleImporter ||
      manifest.song_count != kSongCount) {
    status.detail = "the imported soundtrack was made by an incompatible version";
    return status;
  }

  std::vector<std::uint8_t> soundset_bytes;
  if (!ReadFileCapped(folder / kSoundSetFile, kMaxSoundSetFile, soundset_bytes)) {
    status.detail = "the sound set cannot be read";
    return status;
  }
  if (trg::update::Sha256Hex(AsText(soundset_bytes)) != manifest.soundset_sha256) {
    status.detail = "the sound set does not match its manifest";
    return status;
  }
  SoundSet set;
  const SoundSetError error = DeserializeSoundSet(soundset_bytes, set);
  if (error != SoundSetError::kOk) {
    status.detail = SoundSetErrorText(error);
    return status;
  }
  if (SoundtrackFingerprint(set) != manifest.soundtrack_fingerprint) {
    status.detail = "the sound set fingerprint does not match its manifest";
    return status;
  }
  status.status = SoundtrackStatus::kReady;
  status.manifest = std::move(manifest);
  if (loaded) {
    *loaded = std::move(set);
  }
  return status;
}

SoundtrackStatus StatusForImport(const ImportResult& result) {
  if (result.ok) {
    return SoundtrackStatus::kReady;
  }
  switch (result.failure) {
    case ImportFailure::kInvalidRom:
    case ImportFailure::kUnsupportedRom:
    case ImportFailure::kUnsupportedSoundtrack:
      return SoundtrackStatus::kUnsupportedRom;
    default:
      return SoundtrackStatus::kImportFailed;
  }
}

bool RemoveSoundtrack(const fs::path& user_data_root, std::string& error, const ImportFileOps& ops) {
  if (user_data_root.empty()) {
    error = "no user data folder";
    return false;
  }
  bool ok = true;
  for (const fs::path& path :
       {SoundtrackFolder(user_data_root), Sibling(user_data_root, kStagingSuffix), Sibling(user_data_root, kBackupSuffix)}) {
    std::error_code ec;
    ops.remove_all(path, ec);
    if (ec) {
      error = "cannot remove " + path.filename().string();
      ok = false;
    }
  }
  return ok;
}

}  // namespace tmnt::classic
