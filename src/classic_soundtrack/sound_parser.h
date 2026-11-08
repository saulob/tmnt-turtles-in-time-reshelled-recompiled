#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "classic_soundtrack/soundset.h"

namespace tmnt::classic {

enum class ParseCategory {
  kOk,
  kStructure,  // the sound tables are not where, or what, they should be
  kData,       // a table is right but its compressed or sample data is bad
};

struct ParseResult {
  ParseCategory category = ParseCategory::kStructure;
  std::string detail;
  SoundSet set;
};

// rom is the normalized 1 MiB image.
ParseResult ParseSoundSet(std::span<const std::uint8_t> rom);

}  // namespace tmnt::classic
