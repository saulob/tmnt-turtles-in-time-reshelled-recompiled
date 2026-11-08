#include "classic_soundtrack/sound_parser.h"

#include <algorithm>

#include "classic_soundtrack/konami_lz.h"
#include "classic_soundtrack/snes_rom.h"

namespace tmnt::classic {
namespace {

constexpr std::uint8_t kTableBank = 0x82;
constexpr std::uint16_t kBootListAddress = 0xB9BD;
constexpr std::uint16_t kSongTableAddress = 0xB8E8;
constexpr std::uint16_t kPackTableAddress = 0xB95D;
constexpr std::uint8_t kStageTableBank = 0x81;
constexpr std::uint16_t kStageTableAddress = 0x9409;

constexpr std::uint8_t kUploadOpcode = 0x02;
constexpr size_t kMaxBootBlocks = 16;
constexpr size_t kPackRecordSize = 8;
constexpr size_t kPointerSize = 3;
constexpr std::uint8_t kRawFlag = 0x80;
constexpr std::uint8_t kPackFlag = 0x40;
constexpr std::uint8_t kRomBankMask = 0x3F;
constexpr size_t kLongestStream = 0x10000;
constexpr size_t kStagesWithPacks = 10;
constexpr size_t kTechnodromeStage = 3;
constexpr size_t kTechnodromeElevatorEntry = 11;

struct Failure {
  ParseCategory category;
  std::string detail;
};

struct Pointer {
  std::uint16_t address = 0;
  std::uint8_t bank_byte = 0;
};

class Parser {
 public:
  explicit Parser(std::span<const std::uint8_t> rom) : reader_(rom) {}

  bool Run(SoundSet& set, Failure& failure) {
    failure_ = &failure;
    return ParseBoot(set.boot) && ParseSongs(set.songs) && ParsePacks(set.packs) && ParseStages(set) &&
           AssociatePacks(set);
  }

 private:
  bool Fail(ParseCategory category, std::string detail) {
    *failure_ = {category, std::move(detail)};
    return false;
  }

  bool ReadPointer(std::uint8_t bank, std::uint16_t address, Pointer& pointer) {
    return reader_.ReadU16(bank, address, pointer.address) &&
           reader_.ReadU8(bank, std::uint16_t(address + 2), pointer.bank_byte);
  }

  bool LoadBlock(const Pointer& pointer, bool raw, size_t max_size, std::vector<std::uint8_t>& out) {
    const std::uint8_t rom_bank = pointer.bank_byte & kRomBankMask;
    if (raw) {
      std::uint16_t length = 0;
      std::span<const std::uint8_t> payload;
      if (!reader_.ReadU16(rom_bank, pointer.address, length) || length <= 2 ||
          !reader_.SliceAt(rom_bank, pointer.address, length, payload)) {
        return Fail(ParseCategory::kStructure, "raw block is outside the ROM");
      }
      if (size_t(length) - 2 > max_size) {
        return Fail(ParseCategory::kData, "raw block is larger than its destination");
      }
      out.assign(payload.begin() + 2, payload.end());
      return true;
    }
    size_t offset = 0;
    std::span<const std::uint8_t> stream;
    if (!LoRomOffset(rom_bank, pointer.address, reader_.size(), offset) ||
        !reader_.Slice(offset, std::min(kLongestStream, reader_.size() - offset), stream)) {
      return Fail(ParseCategory::kStructure, "compressed block is outside the ROM");
    }
    LzResult result = KonamiLzDecompress(stream, max_size);
    if (result.status != LzStatus::kOk) {
      return Fail(ParseCategory::kData, "compressed block does not decode");
    }
    out = std::move(result.data);
    return true;
  }

  bool ParseBoot(std::vector<AramBlock>& boot) {
    std::uint8_t opcode = 0;
    if (!reader_.ReadU8(kTableBank, kBootListAddress, opcode) || opcode != kUploadOpcode) {
      return Fail(ParseCategory::kStructure, "boot upload list not found");
    }
    std::uint16_t cursor = kBootListAddress + 2;
    for (size_t i = 0; i < kMaxBootBlocks; ++i) {
      std::uint16_t dest = 0;
      if (!reader_.ReadU16(kTableBank, cursor, dest)) {
        return Fail(ParseCategory::kStructure, "boot upload list is cut off");
      }
      if (dest & 1) {
        return boot.empty() ? Fail(ParseCategory::kStructure, "boot upload list is empty") : true;
      }
      Pointer pointer;
      if (!ReadPointer(kTableBank, std::uint16_t(cursor + 2), pointer) || (pointer.bank_byte & kPackFlag)) {
        return Fail(ParseCategory::kStructure, "boot upload entry is invalid");
      }
      AramBlock block;
      block.dest = dest;
      if (!LoadBlock(pointer, pointer.bank_byte & kRawFlag, kAramSize - dest, block.data)) {
        return false;
      }
      boot.push_back(std::move(block));
      cursor = std::uint16_t(cursor + 5);
    }
    return Fail(ParseCategory::kStructure, "boot upload list has no end");
  }

  bool ParseSongs(std::vector<Song>& songs) {
    for (size_t i = 0; i < kSongCount; ++i) {
      Pointer pointer;
      if (!ReadPointer(kTableBank, std::uint16_t(kSongTableAddress + i * kPointerSize), pointer) ||
          (pointer.bank_byte & kPackFlag)) {
        return Fail(ParseCategory::kStructure, "song table entry is invalid");
      }
      // Songs are always compressed; bit 7 only selects the 8-track command.
      Song song;
      song.command = (pointer.bank_byte & kRawFlag) ? kSongCommand8Track : kSongCommand6Track;
      if (!LoadBlock(pointer, false, kSongRegionSize, song.data)) {
        return false;
      }
      songs.push_back(std::move(song));
    }
    return true;
  }

  bool ParsePacks(std::vector<StagePack>& packs) {
    for (size_t i = 0; i < kPackCount; ++i) {
      const std::uint16_t record = std::uint16_t(kPackTableAddress + i * kPackRecordSize);
      Pointer samples;
      Pointer directory;
      StagePack pack;
      if (!ReadPointer(kTableBank, record, samples) ||
          !ReadPointer(kTableBank, std::uint16_t(record + kPointerSize), directory) ||
          !reader_.ReadU8(kTableBank, std::uint16_t(record + 2 * kPointerSize), pack.command) ||
          !(samples.bank_byte & kPackFlag)) {
        return Fail(ParseCategory::kStructure, "stage pack record is invalid");
      }
      const bool raw = samples.bank_byte & kRawFlag;
      pack.blocks.resize(2);
      pack.blocks[0].dest = kPackSamplesAddress;
      pack.blocks[1].dest = kDirectoryAddress;
      if (!LoadBlock(samples, raw, kPackSamplesLimit, pack.blocks[0].data) ||
          !LoadBlock(directory, raw, kDirectoryLimit, pack.blocks[1].data)) {
        return false;
      }
      packs.push_back(std::move(pack));
    }
    return true;
  }

  bool ParseStages(SoundSet& set) {
    for (size_t i = 0; i < kStageCount; ++i) {
      std::uint16_t value = 0;
      if (!reader_.ReadU16(kStageTableBank, std::uint16_t(kStageTableAddress + i * 2), value)) {
        return Fail(ParseCategory::kStructure, "stage music table is outside the ROM");
      }
      if (value == kNoId) {
        set.stage_songs.push_back(kNoId);
      } else if (value % 3 == 0 && value / 3 < kSongCount) {
        set.stage_songs.push_back(std::uint16_t(value / 3));
      } else {
        return Fail(ParseCategory::kStructure, "stage music table holds an invalid song");
      }
    }
    return true;
  }

  // Stage s loads pack s+1; a song belongs to the first entry that plays it. Entry 11 is not a stage: the Technodrome
  // stage picks it for its elevator sections, so that stage's pack is the resident one.
  bool AssociatePacks(SoundSet& set) {
    for (size_t entry = 0; entry < set.stage_songs.size(); ++entry) {
      const std::uint16_t song = set.stage_songs[entry];
      const size_t stage = entry == kTechnodromeElevatorEntry ? kTechnodromeStage : entry;
      const size_t pack = stage + 1;
      if (song != kNoId && stage < kStagesWithPacks && set.songs[song].pack_id == kNoId) {
        set.songs[song].pack_id = std::uint16_t(pack);
      }
    }
    return true;
  }

  RomReader reader_;
  Failure* failure_ = nullptr;
};

}  // namespace

ParseResult ParseSoundSet(std::span<const std::uint8_t> rom) {
  ParseResult result;
  Failure failure{ParseCategory::kStructure, {}};
  Parser parser(rom);
  if (!parser.Run(result.set, failure)) {
    result.category = failure.category;
    result.detail = std::move(failure.detail);
    result.set = {};
    return result;
  }
  switch (ValidateSoundSet(result.set)) {
    case SoundSetError::kOk:
      result.category = ParseCategory::kOk;
      break;
    case SoundSetError::kBadBrr:
      result.category = ParseCategory::kData;
      result.detail = "sample chain is invalid";
      break;
    default:
      result.category = ParseCategory::kStructure;
      result.detail = "sound tables are inconsistent";
      break;
  }
  if (result.category != ParseCategory::kOk) {
    result.set = {};
  }
  return result;
}

}  // namespace tmnt::classic
