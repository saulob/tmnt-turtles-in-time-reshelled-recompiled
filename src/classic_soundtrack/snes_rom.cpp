#include "classic_soundtrack/snes_rom.h"

namespace tmnt::classic {
namespace {

constexpr size_t kHeaderOffset = 0x7FC0;
constexpr size_t kTitleLength = 21;
constexpr size_t kMapModeAt = 0x7FD5;
constexpr size_t kRomSizeAt = 0x7FD7;
constexpr size_t kRegionAt = 0x7FD9;
constexpr size_t kVersionAt = 0x7FDB;
constexpr size_t kComplementAt = 0x7FDC;
constexpr size_t kChecksumAt = 0x7FDE;
constexpr std::uint8_t kRomSizeExponent = 10;

std::uint16_t U16(std::span<const std::uint8_t> rom, size_t at) {
  return std::uint16_t(rom[at] | rom[at + 1] << 8);
}

bool IsLoRomMapMode(std::uint8_t mode) {
  return (mode & 0xEF) == 0x20;
}

}  // namespace

RomStatus NormalizeRom(std::span<const std::uint8_t> file, std::span<const std::uint8_t>& rom) {
  if (file.size() == kRomSize) {
    rom = file;
    return RomStatus::kOk;
  }
  if (file.size() == kRomSize + kCopierHeaderSize) {
    rom = file.subspan(kCopierHeaderSize);
    return RomStatus::kOk;
  }
  return RomStatus::kInvalidSize;
}

RomStatus ValidateLoRomHeader(std::span<const std::uint8_t> rom, RomHeaderInfo* info) {
  if (rom.size() != kRomSize) {
    return RomStatus::kInvalidSize;
  }
  if (!IsLoRomMapMode(rom[kMapModeAt]) || rom[kRomSizeAt] != kRomSizeExponent) {
    return RomStatus::kInvalidHeader;
  }
  const std::uint16_t complement = U16(rom, kComplementAt);
  const std::uint16_t checksum = U16(rom, kChecksumAt);
  if (std::uint16_t(complement ^ checksum) != 0xFFFF) {
    return RomStatus::kBadChecksum;
  }
  std::uint32_t sum = 0;
  for (const std::uint8_t byte : rom) {
    sum += byte;
  }
  if (std::uint16_t(sum) != checksum) {
    return RomStatus::kBadChecksum;
  }
  if (info) {
    info->title.clear();
    for (size_t i = 0; i < kTitleLength; ++i) {
      const std::uint8_t c = rom[kHeaderOffset + i];
      info->title += (c >= 0x20 && c < 0x7F) ? char(c) : ' ';
    }
    while (!info->title.empty() && info->title.back() == ' ') {
      info->title.pop_back();
    }
    info->map_mode = rom[kMapModeAt];
    info->region = rom[kRegionAt];
    info->version = rom[kVersionAt];
  }
  return RomStatus::kOk;
}

bool LoRomOffset(std::uint8_t bank, std::uint16_t address, size_t rom_size, size_t& offset) {
  if (address < 0x8000) {
    return false;
  }
  const size_t at = size_t(bank & 0x7F) * 0x8000 + (address - 0x8000);
  if (at >= rom_size) {
    return false;
  }
  offset = at;
  return true;
}

bool RomReader::Slice(size_t offset, size_t length, std::span<const std::uint8_t>& out) const {
  if (offset > rom_.size() || length > rom_.size() - offset) {
    return false;
  }
  out = rom_.subspan(offset, length);
  return true;
}

bool RomReader::SliceAt(std::uint8_t bank, std::uint16_t address, size_t length,
                        std::span<const std::uint8_t>& out) const {
  size_t offset = 0;
  return LoRomOffset(bank, address, rom_.size(), offset) && Slice(offset, length, out);
}

bool RomReader::ReadU8(std::uint8_t bank, std::uint16_t address, std::uint8_t& value) const {
  std::span<const std::uint8_t> bytes;
  if (!SliceAt(bank, address, 1, bytes)) {
    return false;
  }
  value = bytes[0];
  return true;
}

bool RomReader::ReadU16(std::uint8_t bank, std::uint16_t address, std::uint16_t& value) const {
  std::span<const std::uint8_t> bytes;
  if (!SliceAt(bank, address, 2, bytes)) {
    return false;
  }
  value = std::uint16_t(bytes[0] | bytes[1] << 8);
  return true;
}

const char* RegionLabel(std::uint8_t region_byte) {
  switch (region_byte) {
    case 0: return "Japan";
    case 1: return "North America";
    case 2: return "Europe";
    default: return "Other";
  }
}

}  // namespace tmnt::classic
