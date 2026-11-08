// Classic Soundtrack importer: Konami LZ, ROM validation, bounded reads, sound set format, manifest, atomic import,
// status, remove and the repository safeguards. Uses synthetic ROMs only. Run: tmnt_classic_soundtrack_tests

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "classic_soundtrack/importer.h"
#include "classic_soundtrack_fixture.h"
#include "classic_soundtrack/konami_lz.h"
#include "classic_soundtrack/known_roms.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack/snes_rom.h"
#include "classic_soundtrack/sound_parser.h"
#include "classic_soundtrack/soundset.h"

namespace {

namespace fs = std::filesystem;
using namespace tmnt::classic;
using namespace classic_fixture;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = fs::temp_directory_path() / ("tmnt_classic_soundtrack_tests_" + std::to_string(++counter) + "_" +
                                         std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xFFFFFF));
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// ---- Konami LZ -------------------------------------------------------------------------------------------------

void TestLz() {
  std::printf("Konami LZ: literals, ring copies, fills, malformed streams\n");
  {
    const Bytes stream = {0x06, 0x00, 0x83, 'A', 'B', 'C'};
    const LzResult result = KonamiLzDecompress(stream, 64);
    CHECK(result.status == LzStatus::kOk);
    CHECK((result.data == Bytes{'A', 'B', 'C'}));
  }
  {
    const Bytes stream = {0x08, 0x00, 0x83, 'A', 'B', 'C', 0x0B, 0xDF};
    const LzResult result = KonamiLzDecompress(stream, 64);
    CHECK(result.status == LzStatus::kOk);
    CHECK((result.data == Bytes{'A', 'B', 'C', 'A', 'B', 'C', 'A'}));
  }
  {
    const LzResult fill = KonamiLzDecompress(Bytes{0x04, 0x00, 0xC3, 0x7F}, 64);
    CHECK(fill.status == LzStatus::kOk && (fill.data == Bytes(5, 0x7F)));
    const LzResult zeros = KonamiLzDecompress(Bytes{0x03, 0x00, 0xE1}, 64);
    CHECK(zeros.status == LzStatus::kOk && (zeros.data == Bytes(3, 0)));
    const LzResult long_zeros = KonamiLzDecompress(Bytes{0x04, 0x00, 0xFF, 0x05}, 64);
    CHECK(long_zeros.status == LzStatus::kOk && (long_zeros.data == Bytes(7, 0)));
    const LzResult interleaved = KonamiLzDecompress(Bytes{0x05, 0x00, 0xA0, 0x11, 0x22}, 64);
    CHECK(interleaved.status == LzStatus::kOk && (interleaved.data == Bytes{0x00, 0x11, 0x00, 0x22}));
  }
  CHECK(KonamiLzDecompress(Bytes{}, 64).status == LzStatus::kTruncated);
  CHECK(KonamiLzDecompress(Bytes{0x10, 0x00, 0x83}, 64).status == LzStatus::kTruncated);
  CHECK(KonamiLzDecompress(Bytes{0x02, 0x00}, 64).status == LzStatus::kMalformed);
  CHECK(KonamiLzDecompress(Bytes{0x03, 0x00, 0x80}, 64).status == LzStatus::kMalformed);
  CHECK(KonamiLzDecompress(Bytes{0x04, 0x00, 0x85, 'A'}, 64).status == LzStatus::kTruncated);
  CHECK(KonamiLzDecompress(Bytes{0x03, 0x00, 0x05}, 64).status == LzStatus::kTruncated);
  CHECK(KonamiLzDecompress(Bytes{0x03, 0x00, 0xC3}, 64).status == LzStatus::kTruncated);
  CHECK(KonamiLzDecompress(Bytes{0x03, 0x00, 0xFF}, 64).status == LzStatus::kTruncated);
  {
    const LzResult overflow = KonamiLzDecompress(Bytes{0x04, 0x00, 0xDF, 0x01}, 10);
    CHECK(overflow.status == LzStatus::kOverflow);
    CHECK(overflow.data.empty());
  }
  {
    const Bytes padded = {0x06, 0x00, 0x83, 'A', 'B', 'C', 0xFF, 0xFF};
    const LzResult result = KonamiLzDecompress(padded, 64);
    CHECK(result.status == LzStatus::kOk && result.data.size() == 3);
  }
  {
    Bytes noise(300);
    for (size_t i = 0; i < noise.size(); ++i) {
      noise[i] = std::uint8_t(i * 31 + 7);
    }
    noise[0] = 0x2C;
    noise[1] = 0x01;
    const LzResult result = KonamiLzDecompress(noise, 1000);
    CHECK(result.status != LzStatus::kOk ? result.data.empty() : result.data.size() <= 1000);
  }
}

// ---- ROM validation and bounded reads --------------------------------------------------------------------------

void TestRom() {
  std::printf("ROM: copier header, size, LoROM header, checksum, bounded reads\n");
  const Bytes rom = RomBuilder().rom();
  std::span<const std::uint8_t> view;

  CHECK(NormalizeRom(rom, view) == RomStatus::kOk && view.size() == kRomSize && view.data() == rom.data());
  Bytes headered(kCopierHeaderSize, 0xAA);
  headered.insert(headered.end(), rom.begin(), rom.end());
  CHECK(NormalizeRom(headered, view) == RomStatus::kOk && view.size() == kRomSize);
  CHECK(view.data() == headered.data() + kCopierHeaderSize);
  CHECK(ValidateLoRomHeader(view) == RomStatus::kOk);

  CHECK(NormalizeRom(Bytes{}, view) == RomStatus::kInvalidSize);
  CHECK(NormalizeRom(Bytes(kRomSize - 1), view) == RomStatus::kInvalidSize);
  CHECK(NormalizeRom(Bytes(kRomSize + 1), view) == RomStatus::kInvalidSize);
  CHECK(NormalizeRom(Bytes(2 * kRomSize), view) == RomStatus::kInvalidSize);

  RomHeaderInfo info;
  CHECK(ValidateLoRomHeader(rom, &info) == RomStatus::kOk);
  CHECK(info.title == "SYNTHETIC TEST ROM" && info.map_mode == 0x20 && info.region == 1);

  Bytes wrong_mode = rom;
  wrong_mode[0x7FD5] = 0x21;
  CHECK(ValidateLoRomHeader(wrong_mode) == RomStatus::kInvalidHeader);
  Bytes wrong_size = rom;
  wrong_size[0x7FD7] = 9;
  CHECK(ValidateLoRomHeader(wrong_size) == RomStatus::kInvalidHeader);
  Bytes fast = rom;
  fast[0x7FD5] = 0x30;
  FixChecksum(fast);
  CHECK(ValidateLoRomHeader(fast) == RomStatus::kOk);
  CHECK(ValidateLoRomHeader(Bytes(kRomSize, 0)) == RomStatus::kInvalidHeader);
  CHECK(ValidateLoRomHeader(Bytes(100)) == RomStatus::kInvalidSize);

  Bytes bad_complement = rom;
  bad_complement[0x7FDC] ^= 1;
  CHECK(ValidateLoRomHeader(bad_complement) == RomStatus::kBadChecksum);
  Bytes bad_sum = rom;
  bad_sum[0x100] ^= 1;
  CHECK(ValidateLoRomHeader(bad_sum) == RomStatus::kBadChecksum);

  const Bytes tiny(16, 0x5A);
  const RomReader reader(tiny);
  std::span<const std::uint8_t> out;
  CHECK(reader.Slice(0, 16, out) && out.size() == 16);
  CHECK(reader.Slice(16, 0, out));
  CHECK(!reader.Slice(1, 16, out));
  CHECK(!reader.Slice(17, 0, out));
  CHECK(!reader.Slice(8, SIZE_MAX, out));
  CHECK(!reader.Slice(SIZE_MAX, 2, out));
  CHECK(!reader.SliceAt(0x80, 0x7FFF, 1, out));
  CHECK(reader.SliceAt(0x80, 0x8000, 16, out));
  CHECK(!reader.SliceAt(0x80, 0x8000, 17, out));
  CHECK(!reader.SliceAt(0x81, 0x8000, 1, out));
  std::uint16_t word = 0;
  std::uint8_t byte = 0;
  CHECK(reader.ReadU16(0x80, 0x8000, word) && word == 0x5A5A);
  CHECK(!reader.ReadU16(0x80, 0x800F, word));
  CHECK(reader.ReadU8(0x80, 0x800F, byte) && byte == 0x5A);
  CHECK(!reader.ReadU8(0x80, 0x8010, byte));

  size_t offset = 0;
  CHECK(LoRomOffset(0x82, 0xB9BD, kRomSize, offset) && offset == kBootListOffset);
  CHECK(LoRomOffset(0x02, 0x8000, kRomSize, offset) && offset == 0x10000);
  CHECK(!LoRomOffset(0x20, 0x8000, kRomSize, offset));
  CHECK(!LoRomOffset(0x82, 0x1234, kRomSize, offset));
}

// ---- Parser ----------------------------------------------------------------------------------------------------

void TestParser() {
  std::printf("parser: tables, pack association, structure and data failures\n");
  const Bytes rom = RomBuilder().rom();
  ParseResult parsed = ParseSoundSet(rom);
  CHECK(parsed.category == ParseCategory::kOk);
  CHECK(parsed.set.songs.size() == kSongCount && parsed.set.packs.size() == kPackCount);
  CHECK(parsed.set.boot.size() == 4 && parsed.set.boot[0].dest == kDriverAddress && parsed.set.boot[0].data.size() == 40);
  CHECK(parsed.set.songs[0].command == kSongCommand6Track && parsed.set.songs[1].command == kSongCommand8Track);
  CHECK(parsed.set.songs[1].data == Pattern(21, 1));
  CHECK(parsed.set.songs[0].data == Pattern(20, 0));
  CHECK(parsed.set.packs[3].command == 0x73 && parsed.set.packs[3].blocks[0].dest == kPackSamplesAddress);
  CHECK(parsed.set.stage_songs.size() == kStageCount && parsed.set.stage_songs[1] == 3 && parsed.set.stage_songs[11] == kNoId);
  CHECK(parsed.set.songs[0].pack_id == 1 && parsed.set.songs[3].pack_id == 2 && parsed.set.songs[9].pack_id == 4);
  CHECK(parsed.set.songs[30].pack_id == kNoId);

  Bytes elevator = rom;
  elevator[kStageTableOffset + 11 * 2] = 33 * 3;
  elevator[kStageTableOffset + 11 * 2 + 1] = 0;
  const ParseResult with_elevator = ParseSoundSet(elevator);
  CHECK(with_elevator.category == ParseCategory::kOk && with_elevator.set.songs[33].pack_id == 4);
  CHECK(parsed.set.songs[1].pack_id == kNoId);

  const auto expect = [&](size_t at, std::uint8_t value, ParseCategory category) {
    Bytes broken = rom;
    broken[at] = value;
    return ParseSoundSet(broken).category == category;
  };
  CHECK(expect(kBootListOffset, 0x05, ParseCategory::kStructure));
  CHECK(expect(kStageTableOffset, 4, ParseCategory::kStructure));
  CHECK(expect(kSongTableOffset + 2, 0x3F, ParseCategory::kStructure));
  CHECK(expect(kPackTableOffset + 2, 0x80, ParseCategory::kStructure));
  CHECK(expect(kBootListOffset + 6, 0x3F, ParseCategory::kStructure));
  CHECK(ParseSoundSet(Bytes(kRomSize, 0)).category == ParseCategory::kStructure);

  Bytes lz_cut = rom;
  lz_cut[0x20000] = 0xFF;
  lz_cut[0x20001] = 0xFF;
  CHECK(ParseSoundSet(lz_cut).category != ParseCategory::kOk);

  Bytes bad_brr = rom;
  for (size_t i = 0x20000; i + 12 < 0x30000; ++i) {
    if (bad_brr[i] == 0x00 && bad_brr[i + 1] == 0x3E && bad_brr[i + 2] == 0x00 && bad_brr[i + 3] == 0x3E && bad_brr[i + 4] == 0x09) {
      bad_brr[i] = 0xF8;
      bad_brr[i + 1] = 0xFF;
    }
  }
  CHECK(ParseSoundSet(bad_brr).category == ParseCategory::kData);

  Bytes too_big = rom;
  const size_t oversized = 0x70000;
  too_big[oversized] = 32;
  too_big[oversized + 1] = 0;
  for (size_t i = 0; i < 15; ++i) {
    too_big[oversized + 2 + i * 2] = 0xFF;
    too_big[oversized + 3 + i * 2] = 0xFF;
  }
  too_big[kSongTableOffset + 3] = 0x00;
  too_big[kSongTableOffset + 4] = 0x80;
  too_big[kSongTableOffset + 5] = 0x80 | std::uint8_t(oversized / 0x8000);
  CHECK(ParseSoundSet(too_big).category == ParseCategory::kData);
}

// ---- Sound set format ------------------------------------------------------------------------------------------

bool SameSet(const SoundSet& a, const SoundSet& b) {
  return SerializeSoundSet(a) == SerializeSoundSet(b);
}

void TestSoundSet() {
  std::printf("sound set: serialization, version, truncation, section ranges, validation\n");
  const SoundSet set = ParseSoundSet(RomBuilder().rom()).set;
  const Bytes bytes = SerializeSoundSet(set);
  CHECK(bytes.size() > 12 && bytes[0] == 'T' && bytes[1] == 'M' && bytes[2] == 'C' && bytes[3] == 'S');
  CHECK(SerializeSoundSet(set) == bytes);

  SoundSet loaded;
  CHECK(DeserializeSoundSet(bytes, loaded) == SoundSetError::kOk);
  CHECK(SameSet(set, loaded));
  CHECK(SoundtrackFingerprint(loaded) == SoundtrackFingerprint(set));
  CHECK(SoundtrackFingerprint(set).size() == 64);

  Bytes versioned = bytes;
  versioned[4] = 2;
  CHECK(DeserializeSoundSet(versioned, loaded) == SoundSetError::kUnsupportedVersion);
  Bytes magic = bytes;
  magic[0] = 'X';
  CHECK(DeserializeSoundSet(magic, loaded) == SoundSetError::kBadMagic);

  CHECK(DeserializeSoundSet(Bytes{}, loaded) == SoundSetError::kTruncated);
  CHECK(DeserializeSoundSet(Bytes(bytes.begin(), bytes.end() - 1), loaded) == SoundSetError::kTruncated);
  bool every_prefix_rejected = true;
  for (size_t length = 0; length < bytes.size(); length += 997) {
    every_prefix_rejected = every_prefix_rejected &&
                            DeserializeSoundSet(Bytes(bytes.begin(), bytes.begin() + length), loaded) != SoundSetError::kOk;
  }
  CHECK(every_prefix_rejected);
  Bytes trailing = bytes;
  trailing.push_back(0);
  CHECK(DeserializeSoundSet(trailing, loaded) != SoundSetError::kOk);

  Bytes offset_past_end = bytes;
  offset_past_end[12 + 4] = 0xFF;
  offset_past_end[12 + 5] = 0xFF;
  offset_past_end[12 + 6] = 0xFF;
  CHECK(DeserializeSoundSet(offset_past_end, loaded) == SoundSetError::kBadSection);
  Bytes size_past_end = bytes;
  size_past_end[12 + 8] = 0xFF;
  size_past_end[12 + 9] = 0xFF;
  size_past_end[12 + 10] = 0xFF;
  CHECK(DeserializeSoundSet(size_past_end, loaded) == SoundSetError::kBadSection);
  Bytes wrong_id = bytes;
  wrong_id[12] = 9;
  CHECK(DeserializeSoundSet(wrong_id, loaded) == SoundSetError::kBadSection);
  Bytes section_count = bytes;
  section_count[6] = 3;
  CHECK(DeserializeSoundSet(section_count, loaded) == SoundSetError::kBadSection);

  for (size_t at = 0; at < bytes.size(); at += 1531) {
    Bytes damaged = bytes;
    damaged[at] ^= 0xFF;
    SoundSet scratch;
    const SoundSetError error = DeserializeSoundSet(damaged, scratch);
    CHECK(error == SoundSetError::kOk || scratch.songs.empty());
  }

  SoundSet broken = set;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kOk);
  broken.boot[1].dest = 0xFFF0;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBlock);
  broken = set;
  broken.boot[2].dest = broken.boot[1].dest;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBlock);
  broken = set;
  broken.boot.erase(broken.boot.begin());
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBlock);
  broken = set;
  broken.songs.pop_back();
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadCount);
  broken = set;
  broken.songs[3].pack_id = 99;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadSong);
  broken = set;
  broken.songs[3].command = 0x81;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadSong);
  broken = set;
  broken.songs[3].data.assign(kSongRegionSize + 1, 1);
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadSong);
  broken = set;
  broken.stage_songs[2] = 500;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadSong);
  broken = set;
  broken.packs[4].blocks[0].data.assign(kPackSamplesLimit + 1, 1);
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBlock);
  broken = set;
  broken.packs[4].blocks[1].dest = 0xFE04;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBlock);
  broken = set;
  broken.packs[5].blocks[0].data[0] = 0xD1;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kOk);
  broken = set;
  broken.packs[5].blocks[0].data[0] = 0x00;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kOk);
  broken = set;
  broken.packs[5].blocks[1].data[0] = 0xF8;
  broken.packs[5].blocks[1].data[1] = 0xFF;
  CHECK(ValidateSoundSet(broken) == SoundSetError::kBadBrr);

  const Bytes aram = BuildAramImage(set, 2, 5);
  CHECK(aram.size() == kAramSize);
  CHECK(std::equal(set.songs[5].data.begin(), set.songs[5].data.end(), aram.begin() + kSongAddress));
  CHECK(std::equal(set.boot[0].data.begin(), set.boot[0].data.end(), aram.begin() + kDriverAddress));
  CHECK(std::equal(set.packs[2].blocks[1].data.begin(), set.packs[2].blocks[1].data.end(), aram.begin() + kDirectoryAddress));
  CHECK(BuildAramImage(set, 99, 99) == BuildAramImage(set));
}

// ---- Manifest --------------------------------------------------------------------------------------------------

ClassicSoundtrackManifest SampleManifest() {
  ClassicSoundtrackManifest manifest;
  manifest.format_version = 1;
  manifest.importer_version = 1;
  manifest.source_rom_sha256 = std::string(64, 'a');
  manifest.source_region = "USA";
  manifest.soundtrack_fingerprint = std::string(64, 'b');
  manifest.soundset_sha256 = std::string(64, 'c');
  manifest.song_count = 2;
  manifest.songs.push_back({0, "ROOF", 0x80, 1});
  manifest.songs.push_back({1, "", 0xA0, 0xFFFF});
  return manifest;
}

void TestManifest() {
  std::printf("manifest: keys, song table, round trip, rejection\n");
  const ClassicSoundtrackManifest manifest = SampleManifest();
  const std::string text = SerializeManifest(manifest);
  CHECK(Has(text, "format_version = 1"));
  CHECK(Has(text, "importer_version = 1"));
  CHECK(text.find("source_rom_sha256 = \"" + std::string(64, 'a') + "\"") != std::string::npos);
  CHECK(Has(text, "source_region = \"USA\""));
  CHECK(text.find("soundtrack_fingerprint = \"" + std::string(64, 'b') + "\"") != std::string::npos);
  CHECK(text.find("soundset_sha256 = \"" + std::string(64, 'c') + "\"") != std::string::npos);
  CHECK(Has(text, "song_count = 2"));
  CHECK(Has(text, "[[song]]") && Has(text, "label = \"ROOF\"") && Has(text, "command = 0x80") && Has(text, "stage_pack = 1"));
  CHECK(Has(text, "command = 0xA0"));
  CHECK(std::count(text.begin(), text.end(), '[') == 4);
  CHECK(text.find("stage_pack", text.find("0xA0")) == std::string::npos);

  ClassicSoundtrackManifest parsed;
  CHECK(ParseManifest(text, parsed));
  CHECK(parsed.songs.size() == 2 && parsed.songs[0].label == "ROOF" && parsed.songs[0].stage_pack == 1);
  CHECK(parsed.songs[1].label.empty() && parsed.songs[1].command == 0xA0 && parsed.songs[1].stage_pack == 0xFFFF);
  CHECK(parsed.source_rom_sha256 == manifest.source_rom_sha256 && parsed.song_count == 2);
  CHECK(SerializeManifest(parsed) == text);

  std::string without_fingerprint = text;
  without_fingerprint.replace(without_fingerprint.find("soundtrack_fingerprint"), 22, "soundtrack_fingerprinx");
  CHECK(!ParseManifest(without_fingerprint, parsed));
  CHECK(!ParseManifest("", parsed));
  CHECK(!ParseManifest("not toml at all", parsed));
  CHECK(!ParseManifest("format_version = \"one\"\n", parsed));
  std::string no_command = text;
  no_command.erase(no_command.find("command = 0xA0"), 15);
  CHECK(!ParseManifest(no_command, parsed));

  ClassicSoundtrackManifest quoted = manifest;
  quoted.songs[0].label = "A \"B\" \\ C";
  ClassicSoundtrackManifest quoted_back;
  CHECK(ParseManifest(SerializeManifest(quoted), quoted_back) && quoted_back.songs[0].label == quoted.songs[0].label);
}

// ---- Importer, status, remove ----------------------------------------------------------------------------------

std::string FolderFingerprint(const fs::path& root) {
  return ReadText(SoundtrackFolder(root) / kSoundSetFile);
}

void TestImporter() {
  std::printf("importer: validation, unknown soundtrack, labels, known ROM table\n");
  const Bytes rom = RomBuilder().rom();
  const std::string fingerprint = FingerprintOf(rom);
  CHECK(fingerprint.size() == 64);

  RomAnalysis known = AnalyzeRom(rom, fingerprint);
  CHECK(known.failure == ImportFailure::kNone);
  CHECK(known.manifest.song_count == kSongCount && known.manifest.songs.size() == kSongCount);
  CHECK(known.manifest.source_region == kCompatibleRegion);
  CHECK(known.manifest.soundtrack_fingerprint == fingerprint);
  CHECK(known.manifest.format_version == kSoundSetFormatVersion && known.manifest.importer_version == kImporterVersion);
  CHECK(known.manifest.soundset_sha256.size() == 64 && known.manifest.source_rom_sha256.size() == 64);
  CHECK(known.manifest.songs[0].label == "ROOF" && known.manifest.songs[10].label.empty());
  CHECK(known.manifest.songs[0].stage_pack == 1 && known.manifest.songs[1].stage_pack == 0xFFFF);

  CHECK(AnalyzeRom(rom).failure == ImportFailure::kUnsupportedSoundtrack);
  CHECK(AnalyzeRom(rom, std::string(64, '0')).failure == ImportFailure::kUnsupportedSoundtrack);

  Bytes headered(kCopierHeaderSize, 0x11);
  headered.insert(headered.end(), rom.begin(), rom.end());
  const RomAnalysis with_header = AnalyzeRom(headered, fingerprint);
  CHECK(with_header.failure == ImportFailure::kNone && with_header.copier_header);
  CHECK(with_header.manifest.source_rom_sha256 == known.manifest.source_rom_sha256);

  CHECK(AnalyzeRom(Bytes(1000), fingerprint).failure == ImportFailure::kInvalidRom);
  Bytes bad_header = rom;
  bad_header[0x7FD5] = 0x01;
  CHECK(AnalyzeRom(bad_header, fingerprint).failure == ImportFailure::kInvalidRom);
  Bytes bad_sum = rom;
  bad_sum[0x500] ^= 0x10;
  CHECK(AnalyzeRom(bad_sum, fingerprint).failure == ImportFailure::kInvalidRom);
  Bytes unrelated(kRomSize, 0);
  unrelated[0x7FD5] = 0x20;
  unrelated[0x7FD7] = 10;
  FixChecksum(unrelated);
  CHECK(AnalyzeRom(unrelated, fingerprint).failure == ImportFailure::kUnsupportedRom);
  Bytes corrupt = rom;
  for (size_t i = 0x20000; i + 12 < 0x30000; ++i) {
    if (corrupt[i] == 0x00 && corrupt[i + 1] == 0x3E && corrupt[i + 2] == 0x00 && corrupt[i + 3] == 0x3E && corrupt[i + 4] == 0x09) {
      corrupt[i] = 0xF8;
      corrupt[i + 1] = 0xFF;
    }
  }
  FixChecksum(corrupt);
  CHECK(AnalyzeRom(corrupt, fingerprint).failure == ImportFailure::kCorruptSoundtrack);

  CHECK(std::string(ImportFailureText(ImportFailure::kInvalidRom)) == "Invalid ROM");
  CHECK(std::string(ImportFailureText(ImportFailure::kUnsupportedRom)) == "Unsupported ROM");
  CHECK(std::string(ImportFailureText(ImportFailure::kUnsupportedSoundtrack)) == "Unsupported soundtrack");
  CHECK(std::string(ImportFailureText(ImportFailure::kCorruptSoundtrack)) == "Corrupt soundtrack data");
  CHECK(std::string(ImportFailureText(ImportFailure::kImportFailed)) == "Import failed");

  size_t labeled = 0;
  for (size_t id = 0; id < kSongCount; ++id) {
    const std::string_view label = SongLabel(id);
    labeled += label.empty() ? 0 : 1;
    for (size_t other = id + 1; other < kSongCount && !label.empty(); ++other) {
      CHECK(label != SongLabel(other));
    }
  }
  CHECK(labeled == 31);
  CHECK(SongLabel(kSongCount).empty());

  CHECK(kKnownRoms.size() == 3);
  for (const KnownRom& entry : kKnownRoms) {
    CHECK(entry.sha256.size() == 64);
    CHECK(std::all_of(entry.sha256.begin(), entry.sha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK(KnownRomRegion(entry.sha256) == entry.region);
  }
  CHECK(kKnownRoms[0].sha256 != kKnownRoms[1].sha256 && kKnownRoms[1].sha256 != kKnownRoms[2].sha256);
  CHECK(kKnownSoundtrackFingerprint.size() == 64 && kKnownSoundtrackFingerprint.substr(0, 16) == "149c010ed2fe35ed");
  CHECK(KnownRomRegion(std::string(64, '0')).empty());
}

void TestStatusAndImport() {
  std::printf("status, import, atomic failure, remove\n");
  const Bytes rom_a = RomBuilder(0).rom();
  const Bytes rom_b = RomBuilder(5).rom();
  const std::string fingerprint_a = FingerprintOf(rom_a);
  const std::string fingerprint_b = FingerprintOf(rom_b);
  CHECK(fingerprint_a != fingerprint_b);

  TempDir dir;
  const fs::path root = dir.path() / "user_data";

  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kNotImported);
  CHECK(std::string(SoundtrackStatusText(SoundtrackStatus::kNotImported)) == "Not imported");

  const ImportResult first = ImportSoundtrackFromBytes(rom_a, root, {}, fingerprint_a);
  CHECK(first.ok && first.failure == ImportFailure::kNone);
  CHECK(fs::is_regular_file(SoundtrackFolder(root) / kSoundSetFile) && fs::is_regular_file(SoundtrackFolder(root) / kManifestFile));
  CHECK(SoundtrackFolder(root).filename() == "snes_soundtrack");
  CHECK(!fs::exists(root / "snes_soundtrack.new") && !fs::exists(root / "snes_soundtrack.old"));
  for (const auto& entry : fs::directory_iterator(SoundtrackFolder(root))) {
    CHECK(entry.path().extension() != ".part");
  }

  const StatusResult ready = QuerySoundtrackStatus(root);
  CHECK(ready.status == SoundtrackStatus::kReady && ready.manifest.soundtrack_fingerprint == fingerprint_a);
  CHECK(std::string(SoundtrackStatusText(ready.status)) == "Ready");
  CHECK(StatusForImport(first) == SoundtrackStatus::kReady);
  const std::string installed = FolderFingerprint(root);
  const std::string installed_manifest = ReadText(SoundtrackFolder(root) / kManifestFile);

  const auto still_installed = [&]() {
    return QuerySoundtrackStatus(root).status == SoundtrackStatus::kReady && FolderFingerprint(root) == installed &&
           ReadText(SoundtrackFolder(root) / kManifestFile) == installed_manifest && !fs::exists(root / "snes_soundtrack.new") &&
           !fs::exists(root / "snes_soundtrack.old");
  };

  const ImportResult invalid = ImportSoundtrackFromBytes(Bytes(10), root, {}, fingerprint_b);
  CHECK(!invalid.ok && invalid.failure == ImportFailure::kInvalidRom && StatusForImport(invalid) == SoundtrackStatus::kUnsupportedRom);
  CHECK(std::string(SoundtrackStatusText(SoundtrackStatus::kUnsupportedRom)) == "Unsupported ROM");
  CHECK(still_installed());
  const ImportResult unknown = ImportSoundtrackFromBytes(rom_b, root, {}, fingerprint_a);
  CHECK(!unknown.ok && unknown.failure == ImportFailure::kUnsupportedSoundtrack && still_installed());

  ImportFileOps fail_install;
  fail_install.rename = [&](const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (from.filename() == "snes_soundtrack.new" && to == SoundtrackFolder(root)) {
      ec = std::make_error_code(std::errc::permission_denied);
      return;
    }
    fs::rename(from, to, ec);
  };
  const ImportResult install_failed = ImportSoundtrackFromBytes(rom_b, root, fail_install, fingerprint_b);
  CHECK(!install_failed.ok && install_failed.failure == ImportFailure::kImportFailed);
  CHECK(StatusForImport(install_failed) == SoundtrackStatus::kImportFailed);
  CHECK(still_installed());

  ImportFileOps fail_backup;
  fail_backup.rename = [&](const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (from == SoundtrackFolder(root)) {
      ec = std::make_error_code(std::errc::permission_denied);
      return;
    }
    fs::rename(from, to, ec);
  };
  CHECK(ImportSoundtrackFromBytes(rom_b, root, fail_backup, fingerprint_b).failure == ImportFailure::kImportFailed);
  CHECK(still_installed());

  ImportFileOps fail_write;
  fail_write.rename = [&](const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (to.extension() == ".toml" || to.extension() == ".bin") {
      ec = std::make_error_code(std::errc::io_error);
      return;
    }
    fs::rename(from, to, ec);
  };
  CHECK(ImportSoundtrackFromBytes(rom_b, root, fail_write, fingerprint_b).failure == ImportFailure::kImportFailed);
  CHECK(still_installed());

  TempDir blocked;
  const fs::path file_root = blocked.path() / "not_a_folder";
  std::ofstream(file_root) << "x";
  CHECK(ImportSoundtrackFromBytes(rom_a, file_root, {}, fingerprint_a).failure == ImportFailure::kImportFailed);

  TempDir empty_root;
  ImportFileOps fail_first;
  fail_first.rename = [&](const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (from.filename() == "snes_soundtrack.new" && to == SoundtrackFolder(empty_root.path())) {
      ec = std::make_error_code(std::errc::permission_denied);
      return;
    }
    fs::rename(from, to, ec);
  };
  const ImportResult failed_first = ImportSoundtrackFromBytes(rom_a, empty_root.path(), fail_first, fingerprint_a);
  CHECK(failed_first.failure == ImportFailure::kImportFailed);
  CHECK(!fs::exists(SoundtrackFolder(empty_root.path())) && !fs::exists(empty_root.path() / "snes_soundtrack.new"));
  CHECK(QuerySoundtrackStatus(empty_root.path()).status == SoundtrackStatus::kNotImported);

  const ImportResult replaced = ImportSoundtrackFromBytes(rom_b, root, {}, fingerprint_b);
  CHECK(replaced.ok && QuerySoundtrackStatus(root).manifest.soundtrack_fingerprint == fingerprint_b);
  CHECK(!fs::exists(root / "snes_soundtrack.new") && !fs::exists(root / "snes_soundtrack.old"));

  const fs::path rom_file = dir.path() / "my rom.sfc";
  {
    std::ofstream out(rom_file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom_a.data()), std::streamsize(rom_a.size()));
  }
  const fs::path file_root2 = dir.path() / "from_file";
  CHECK(ImportSoundtrack(rom_file, file_root2).failure == ImportFailure::kUnsupportedSoundtrack);
  CHECK(ImportSoundtrack(dir.path() / "missing.sfc", file_root2).failure == ImportFailure::kImportFailed);
  const fs::path big_file = dir.path() / "big.sfc";
  {
    std::ofstream out(big_file, std::ios::binary);
    const Bytes big(kRomSize * 3, 0);
    out.write(reinterpret_cast<const char*>(big.data()), std::streamsize(big.size()));
  }
  CHECK(ImportSoundtrack(big_file, file_root2).failure == ImportFailure::kInvalidRom);
  CHECK(!fs::exists(file_root2 / "snes_soundtrack"));

  const fs::path folder = SoundtrackFolder(root);
  const std::string good_set = ReadText(folder / kSoundSetFile);
  const std::string good_manifest = ReadText(folder / kManifestFile);
  const auto write = [&](const char* name, const std::string& text) {
    std::ofstream out(folder / name, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
  };
  const auto restore = [&]() {
    write(kSoundSetFile, good_set);
    write(kManifestFile, good_manifest);
  };
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kReady);

  std::string flipped = good_set;
  flipped[flipped.size() / 2] ^= 0x01;
  write(kSoundSetFile, flipped);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();

  write(kSoundSetFile, good_set.substr(0, good_set.size() - 10));
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();

  write(kManifestFile, "garbage");
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();

  std::string future = good_manifest;
  future.replace(future.find("format_version = 1"), 18, "format_version = 2");
  write(kManifestFile, future);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();

  std::string wrong_fingerprint = good_manifest;
  wrong_fingerprint.replace(wrong_fingerprint.find("soundtrack_fingerprint = \"") + 26, 4, "0000");
  write(kManifestFile, wrong_fingerprint);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();

  fs::remove(folder / kManifestFile);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();
  fs::remove(folder / kSoundSetFile);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  restore();
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kReady);

  std::ofstream(root / "settings.toml") << "keep";
  fs::create_directories(root / "snes_soundtrack.new");
  fs::create_directories(root / "0000000000000000");
  std::string error;
  CHECK(RemoveSoundtrack(root, error) && error.empty());
  CHECK(!fs::exists(folder) && !fs::exists(root / "snes_soundtrack.new"));
  CHECK(fs::exists(root / "settings.toml") && fs::exists(root / "0000000000000000"));
  CHECK(fs::exists(rom_file));
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kNotImported);
  CHECK(RemoveSoundtrack(root, error));
  CHECK(!RemoveSoundtrack({}, error));
}

// ---- Repository safeguards -------------------------------------------------------------------------------------

void TestSafeguards() {
  std::printf("repository: ignore rules, release checks and source hygiene\n");
  const fs::path root = TMNT_ROOT_DIR;
  const std::string gitignore = ReadText(root / ".gitignore");
  for (const char* pattern : {"*.sfc", "*.smc", "*.swc", "*.fig", "*.spc", "*.rsn", "/game/"}) {
    CHECK(Has(gitignore, pattern));
  }
  const std::string windows = ReadText(root / "tools" / "package-release.ps1");
  const std::string linux_policy = ReadText(root / "tools" / "linux-policy.sh");
  for (const char* needle : {"snes_soundtrack", "soundset.bin", "*.sfc", "*.smc", "*.spc"}) {
    CHECK(Has(windows, needle));
    CHECK(Has(linux_policy, needle));
  }
  CHECK(Has(ReadText(root / "tools" / "test.ps1"), "sfc|smc|swc|fig|spc|rsn"));

  std::size_t files = 0;
  for (const auto& entry : fs::directory_iterator(root / "src" / "classic_soundtrack")) {
    const std::string text = ReadText(entry.path());
    ++files;
    CHECK(text.size() < 40000);
    CHECK(!Has(text, "game/snes"));
    const std::string name = entry.path().filename().string();
    CHECK(!Has(text, "DEBUG_SAULO") || name == "xma_provider.cpp" || name == "xma_provider.h");
  }
  CHECK(files >= 10);
}

}  // namespace

int main() {
  TestLz();
  TestRom();
  TestParser();
  TestSoundSet();
  TestManifest();
  TestImporter();
  TestStatusAndImport();
  TestSafeguards();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
