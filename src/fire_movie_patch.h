#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tmnt {

// Bytes of a movie as lowercase hex: what they must hold and what they become.
struct MovieEdit {
  size_t offset;
  std::string_view before;
  std::string_view after;
};

// A menu movie inside a decompressed world bundle.
struct MenuMovie {
  std::string_view name;
  size_t bundle_bytes;
  size_t offset;
  size_t bytes;
  uint64_t hash;
  std::span<const MovieEdit> edits;
};

// Applies every edit or none: false when one does not fit or a byte is not what it expects.
bool ApplyMovieEdits(uint8_t* movie, size_t size, std::span<const MovieEdit> edits);

std::span<const MenuMovie> MenuMovies();

bool IsMenuBundle(size_t size);

struct BundlePatchResult {
  int patched = 0;
  int failed = 0;
};

// Only an unmodified movie is edited; the others in the bundle are still patched when one does not match.
BundlePatchResult PatchMenuBundle(uint8_t* bundle, size_t size);

}  // namespace tmnt
