#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace tmnt::classic {

inline constexpr size_t kRomSize = 1024 * 1024;
inline constexpr size_t kCopierHeaderSize = 512;

enum class RomStatus { kOk, kInvalidSize, kInvalidHeader, kBadChecksum };

struct RomHeaderInfo {
  std::string title;
  std::uint8_t map_mode = 0;
  std::uint8_t region = 0;
  std::uint8_t version = 0;
};

// Strips a 512-byte copier header; the result is exactly kRomSize bytes.
RomStatus NormalizeRom(std::span<const std::uint8_t> file, std::span<const std::uint8_t>& rom);

RomStatus ValidateLoRomHeader(std::span<const std::uint8_t> rom, RomHeaderInfo* info = nullptr);

// Bounds-checked LoROM reads; every failure is a false return, never an out-of-range access.
class RomReader {
 public:
  explicit RomReader(std::span<const std::uint8_t> rom) : rom_(rom) {}

  bool Slice(size_t offset, size_t length, std::span<const std::uint8_t>& out) const;
  bool SliceAt(std::uint8_t bank, std::uint16_t address, size_t length, std::span<const std::uint8_t>& out) const;
  bool ReadU8(std::uint8_t bank, std::uint16_t address, std::uint8_t& value) const;
  bool ReadU16(std::uint8_t bank, std::uint16_t address, std::uint16_t& value) const;

  size_t size() const { return rom_.size(); }

 private:
  std::span<const std::uint8_t> rom_;
};

bool LoRomOffset(std::uint8_t bank, std::uint16_t address, size_t rom_size, size_t& offset);

const char* RegionLabel(std::uint8_t region_byte);

}  // namespace tmnt::classic
