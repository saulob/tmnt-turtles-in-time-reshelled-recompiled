#include "classic_soundtrack/soundset.h"

#include <algorithm>
#include <cstring>

#include "updater/sha256.h"

namespace tmnt::classic {
namespace {

constexpr char kMagic[4] = {'T', 'M', 'C', 'S'};
constexpr size_t kHeaderSize = 12;
constexpr size_t kDirectoryEntrySize = 12;
constexpr size_t kMaxBootBlocks = 16;
constexpr size_t kPackDirectoryBytes = 12;
constexpr size_t kBrrBlockSize = 9;

enum SectionId : std::uint32_t { kBootSection = 1, kPacksSection, kSongsSection, kStagesSection };
constexpr std::uint32_t kSectionIds[] = {kBootSection, kPacksSection, kSongsSection, kStagesSection};

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(std::uint8_t(value));
  out.push_back(std::uint8_t(value >> 8));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

void PutBlock(std::vector<std::uint8_t>& out, const AramBlock& block) {
  PutU16(out, block.dest);
  PutU32(out, std::uint32_t(block.data.size()));
  out.insert(out.end(), block.data.begin(), block.data.end());
}

void PutBlockList(std::vector<std::uint8_t>& out, const std::vector<AramBlock>& blocks) {
  PutU16(out, std::uint16_t(blocks.size()));
  for (const AramBlock& block : blocks) {
    PutBlock(out, block);
  }
}

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool U8(std::uint8_t& value) {
    if (remaining() < 1) {
      return false;
    }
    value = bytes_[at_++];
    return true;
  }

  bool U16(std::uint16_t& value) {
    if (remaining() < 2) {
      return false;
    }
    value = std::uint16_t(bytes_[at_] | bytes_[at_ + 1] << 8);
    at_ += 2;
    return true;
  }

  bool U32(std::uint32_t& value) {
    if (remaining() < 4) {
      return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= std::uint32_t(bytes_[at_ + i]) << (8 * i);
    }
    at_ += 4;
    return true;
  }

  bool Bytes(size_t length, std::vector<std::uint8_t>& out) {
    if (length > remaining()) {
      return false;
    }
    out.assign(bytes_.begin() + at_, bytes_.begin() + at_ + length);
    at_ += length;
    return true;
  }

  size_t remaining() const { return bytes_.size() - at_; }

 private:
  std::span<const std::uint8_t> bytes_;
  size_t at_ = 0;
};

bool GetBlock(ByteReader& reader, AramBlock& block) {
  std::uint32_t length = 0;
  return reader.U16(block.dest) && reader.U32(length) && reader.Bytes(length, block.data);
}

bool GetBlockList(ByteReader& reader, std::vector<AramBlock>& blocks) {
  std::uint16_t count = 0;
  if (!reader.U16(count) || count > kMaxBootBlocks) {
    return false;
  }
  blocks.assign(count, {});
  return std::all_of(blocks.begin(), blocks.end(), [&](AramBlock& block) { return GetBlock(reader, block); });
}

bool BlockFits(const AramBlock& block) {
  return !block.data.empty() && size_t(block.dest) + block.data.size() <= kAramSize;
}

bool Overlaps(const AramBlock& a, const AramBlock& b) {
  return a.dest < b.dest + b.data.size() && b.dest < a.dest + a.data.size();
}

SoundSetError ValidateBoot(const std::vector<AramBlock>& boot) {
  if (boot.empty() || boot.size() > kMaxBootBlocks) {
    return SoundSetError::kBadCount;
  }
  bool driver = false;
  bool directory = false;
  for (size_t i = 0; i < boot.size(); ++i) {
    if (!BlockFits(boot[i])) {
      return SoundSetError::kBadBlock;
    }
    for (size_t j = i + 1; j < boot.size(); ++j) {
      if (Overlaps(boot[i], boot[j])) {
        return SoundSetError::kBadBlock;
      }
    }
    driver = driver || boot[i].dest == kDriverAddress;
    directory = directory || boot[i].dest == kDirectoryAddress;
  }
  return driver && directory ? SoundSetError::kOk : SoundSetError::kBadBlock;
}

SoundSetError ValidatePackShape(const StagePack& pack) {
  if (pack.blocks.size() != 2) {
    return SoundSetError::kBadBlock;
  }
  const AramBlock& samples = pack.blocks[0];
  const AramBlock& directory = pack.blocks[1];
  const bool ok = samples.dest == kPackSamplesAddress && !samples.data.empty() &&
                  samples.data.size() <= kPackSamplesLimit && directory.dest == kDirectoryAddress &&
                  !directory.data.empty() && directory.data.size() <= kDirectoryLimit &&
                  directory.data.size() % 4 == 0;
  return ok ? SoundSetError::kOk : SoundSetError::kBadBlock;
}

// A BRR chain is valid when it reaches an end block inside ARAM. Every shift range is legal: the S-DSP clamps
// 13-15, and the game's own samples use 13.
bool BrrChainEnds(const std::vector<std::uint8_t>& aram, size_t start) {
  for (size_t at = start; at + kBrrBlockSize <= aram.size(); at += kBrrBlockSize) {
    if (aram[at] & 1) {
      return true;
    }
  }
  return false;
}

SoundSetError ValidatePackSamples(const SoundSet& set, size_t pack_id) {
  const StagePack& pack = set.packs[pack_id];
  if (pack_id == 0 || pack.blocks[1].data.size() != kPackDirectoryBytes) {
    return SoundSetError::kOk;
  }
  const std::vector<std::uint8_t> aram = BuildAramImage(set, int(pack_id));
  const std::vector<std::uint8_t>& directory = pack.blocks[1].data;
  for (size_t i = 0; i + 4 <= directory.size(); i += 4) {
    const size_t start = directory[i] | directory[i + 1] << 8;
    if (!BrrChainEnds(aram, start)) {
      return SoundSetError::kBadBrr;
    }
  }
  return SoundSetError::kOk;
}

SoundSetError ValidateSongs(const SoundSet& set) {
  if (set.songs.size() != kSongCount) {
    return SoundSetError::kBadCount;
  }
  for (const Song& song : set.songs) {
    const bool command_ok = song.command == kSongCommand6Track || song.command == kSongCommand8Track;
    const bool pack_ok = song.pack_id == kNoId || song.pack_id < kPackCount;
    if (!command_ok || !pack_ok || song.data.empty() || song.data.size() > kSongRegionSize) {
      return SoundSetError::kBadSong;
    }
  }
  return SoundSetError::kOk;
}

SoundSetError ValidateStages(const SoundSet& set) {
  if (set.stage_songs.size() != kStageCount) {
    return SoundSetError::kBadCount;
  }
  const bool ok = std::all_of(set.stage_songs.begin(), set.stage_songs.end(),
                              [](std::uint16_t song) { return song == kNoId || song < kSongCount; });
  return ok ? SoundSetError::kOk : SoundSetError::kBadSong;
}

void AppendAll(std::string& out, const std::vector<std::uint8_t>& bytes) {
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

const char* SoundSetErrorText(SoundSetError error) {
  switch (error) {
    case SoundSetError::kOk: return "ok";
    case SoundSetError::kBadMagic: return "not a sound set file";
    case SoundSetError::kUnsupportedVersion: return "unsupported sound set version";
    case SoundSetError::kTruncated: return "sound set is truncated";
    case SoundSetError::kBadSection: return "sound set section is out of range";
    case SoundSetError::kBadCount: return "sound set has an unexpected item count";
    case SoundSetError::kBadBlock: return "sound set has an invalid sample block";
    case SoundSetError::kBadSong: return "sound set has an invalid song";
    case SoundSetError::kBadBrr: return "sound set has an invalid sample chain";
  }
  return "unknown sound set error";
}

SoundSetError ValidateSoundSet(const SoundSet& set) {
  if (const SoundSetError error = ValidateBoot(set.boot); error != SoundSetError::kOk) {
    return error;
  }
  if (set.packs.size() != kPackCount) {
    return SoundSetError::kBadCount;
  }
  for (const StagePack& pack : set.packs) {
    if (const SoundSetError error = ValidatePackShape(pack); error != SoundSetError::kOk) {
      return error;
    }
  }
  if (const SoundSetError error = ValidateSongs(set); error != SoundSetError::kOk) {
    return error;
  }
  if (const SoundSetError error = ValidateStages(set); error != SoundSetError::kOk) {
    return error;
  }
  for (size_t i = 0; i < set.packs.size(); ++i) {
    if (const SoundSetError error = ValidatePackSamples(set, i); error != SoundSetError::kOk) {
      return error;
    }
  }
  return SoundSetError::kOk;
}

std::vector<std::uint8_t> SerializeSoundSet(const SoundSet& set) {
  std::vector<std::uint8_t> boot;
  PutBlockList(boot, set.boot);

  std::vector<std::uint8_t> packs;
  PutU16(packs, std::uint16_t(set.packs.size()));
  for (const StagePack& pack : set.packs) {
    packs.push_back(pack.command);
    packs.push_back(0);
    PutBlockList(packs, pack.blocks);
  }

  std::vector<std::uint8_t> songs;
  PutU16(songs, std::uint16_t(set.songs.size()));
  for (const Song& song : set.songs) {
    songs.push_back(song.command);
    songs.push_back(0);
    PutU16(songs, song.pack_id);
    PutU32(songs, std::uint32_t(song.data.size()));
    songs.insert(songs.end(), song.data.begin(), song.data.end());
  }

  std::vector<std::uint8_t> stages;
  PutU16(stages, std::uint16_t(set.stage_songs.size()));
  for (const std::uint16_t song : set.stage_songs) {
    PutU16(stages, song);
  }

  const std::vector<std::uint8_t>* sections[] = {&boot, &packs, &songs, &stages};
  const size_t directory_end = kHeaderSize + std::size(kSectionIds) * kDirectoryEntrySize;
  size_t total = directory_end;
  for (const auto* section : sections) {
    total += section->size();
  }

  std::vector<std::uint8_t> out(kMagic, kMagic + 4);
  out.reserve(total);
  PutU16(out, kSoundSetFormatVersion);
  PutU16(out, std::uint16_t(std::size(kSectionIds)));
  PutU32(out, std::uint32_t(total));
  size_t offset = directory_end;
  for (size_t i = 0; i < std::size(kSectionIds); ++i) {
    PutU32(out, kSectionIds[i]);
    PutU32(out, std::uint32_t(offset));
    PutU32(out, std::uint32_t(sections[i]->size()));
    offset += sections[i]->size();
  }
  for (const auto* section : sections) {
    out.insert(out.end(), section->begin(), section->end());
  }
  return out;
}

SoundSetError DeserializeSoundSet(std::span<const std::uint8_t> bytes, SoundSet& set) {
  if (bytes.size() < kHeaderSize) {
    return SoundSetError::kTruncated;
  }
  if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
    return SoundSetError::kBadMagic;
  }
  ByteReader header(bytes.subspan(4, kHeaderSize - 4));
  std::uint16_t version = 0;
  std::uint16_t section_count = 0;
  std::uint32_t total = 0;
  header.U16(version);
  header.U16(section_count);
  header.U32(total);
  if (version != kSoundSetFormatVersion) {
    return SoundSetError::kUnsupportedVersion;
  }
  if (total != bytes.size()) {
    return SoundSetError::kTruncated;
  }
  if (section_count != std::size(kSectionIds)) {
    return SoundSetError::kBadSection;
  }
  const size_t directory_end = kHeaderSize + size_t(section_count) * kDirectoryEntrySize;
  if (bytes.size() < directory_end) {
    return SoundSetError::kTruncated;
  }

  std::span<const std::uint8_t> section_bytes[std::size(kSectionIds)];
  ByteReader directory(bytes.subspan(kHeaderSize, section_count * kDirectoryEntrySize));
  size_t expected_offset = directory_end;
  for (size_t i = 0; i < section_count; ++i) {
    std::uint32_t id = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    directory.U32(id);
    directory.U32(offset);
    directory.U32(size);
    if (id != kSectionIds[i] || offset != expected_offset || size > bytes.size() - offset) {
      return SoundSetError::kBadSection;
    }
    section_bytes[i] = bytes.subspan(offset, size);
    expected_offset += size;
  }
  if (expected_offset != bytes.size()) {
    return SoundSetError::kBadSection;
  }

  SoundSet parsed;
  ByteReader boot(section_bytes[0]);
  if (!GetBlockList(boot, parsed.boot) || boot.remaining() != 0) {
    return SoundSetError::kBadBlock;
  }

  ByteReader packs(section_bytes[1]);
  std::uint16_t pack_count = 0;
  if (!packs.U16(pack_count) || pack_count != kPackCount) {
    return SoundSetError::kBadCount;
  }
  parsed.packs.assign(pack_count, {});
  for (StagePack& pack : parsed.packs) {
    std::uint8_t reserved = 0;
    if (!packs.U8(pack.command) || !packs.U8(reserved) || !GetBlockList(packs, pack.blocks)) {
      return SoundSetError::kBadBlock;
    }
  }
  if (packs.remaining() != 0) {
    return SoundSetError::kBadBlock;
  }

  ByteReader songs(section_bytes[2]);
  std::uint16_t song_count = 0;
  if (!songs.U16(song_count) || song_count != kSongCount) {
    return SoundSetError::kBadCount;
  }
  parsed.songs.assign(song_count, {});
  for (Song& song : parsed.songs) {
    std::uint8_t reserved = 0;
    std::uint32_t length = 0;
    if (!songs.U8(song.command) || !songs.U8(reserved) || !songs.U16(song.pack_id) || !songs.U32(length) ||
        length > kSongRegionSize || !songs.Bytes(length, song.data)) {
      return SoundSetError::kBadSong;
    }
  }
  if (songs.remaining() != 0) {
    return SoundSetError::kBadSong;
  }

  ByteReader stages(section_bytes[3]);
  std::uint16_t stage_count = 0;
  if (!stages.U16(stage_count) || stage_count != kStageCount) {
    return SoundSetError::kBadCount;
  }
  parsed.stage_songs.assign(stage_count, kNoId);
  for (std::uint16_t& song : parsed.stage_songs) {
    if (!stages.U16(song)) {
      return SoundSetError::kBadSong;
    }
  }
  if (stages.remaining() != 0) {
    return SoundSetError::kBadSong;
  }

  if (const SoundSetError error = ValidateSoundSet(parsed); error != SoundSetError::kOk) {
    return error;
  }
  set = std::move(parsed);
  return SoundSetError::kOk;
}

bool WriteAramImage(const SoundSet& set, int pack_id, int song_id, std::span<std::uint8_t> aram) {
  if (aram.size() != kAramSize) {
    return false;
  }
  std::fill(aram.begin(), aram.end(), std::uint8_t(0));
  const auto apply = [&](std::uint16_t dest, const std::vector<std::uint8_t>& data) {
    if (!data.empty() && size_t(dest) + data.size() <= kAramSize) {
      std::copy(data.begin(), data.end(), aram.begin() + dest);
    }
  };
  for (const AramBlock& block : set.boot) {
    apply(block.dest, block.data);
  }
  if (pack_id >= 0 && size_t(pack_id) < set.packs.size()) {
    for (const AramBlock& block : set.packs[size_t(pack_id)].blocks) {
      apply(block.dest, block.data);
    }
  }
  if (song_id >= 0 && size_t(song_id) < set.songs.size()) {
    apply(kSongAddress, set.songs[size_t(song_id)].data);
  }
  return true;
}

std::vector<std::uint8_t> BuildAramImage(const SoundSet& set, int pack_id, int song_id) {
  std::vector<std::uint8_t> aram(kAramSize, 0);
  WriteAramImage(set, pack_id, song_id, aram);
  return aram;
}

std::string SoundtrackFingerprint(const SoundSet& set) {
  std::string stream;
  AppendAll(stream, BuildAramImage(set));
  for (const StagePack& pack : set.packs) {
    for (const AramBlock& block : pack.blocks) {
      AppendAll(stream, block.data);
    }
    stream.push_back(char(pack.command));
  }
  for (const Song& song : set.songs) {
    AppendAll(stream, song.data);
    stream.push_back(char(song.command));
  }
  return trg::update::Sha256Hex(stream);
}

}  // namespace tmnt::classic
