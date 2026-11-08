#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tmnt::classic {

enum class LzStatus { kOk, kTruncated, kOverflow, kMalformed };

struct LzResult {
  LzStatus status = LzStatus::kMalformed;
  std::vector<std::uint8_t> data;
};

// The stream starts with its own little-endian length (header included).
LzResult KonamiLzDecompress(std::span<const std::uint8_t> input, size_t max_output);

}  // namespace tmnt::classic
