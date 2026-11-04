#include "fire_movie_patch.h"

namespace tmnt {
namespace {

constexpr uint64_t kOnlineMenuMovieHash = 0x54BD8CE213024D57ull;

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

bool ApplyMovieBytePatches(uint8_t* movie, size_t size, const MovieBytePatch* patches, size_t count) {
  if (!movie) return false;
  for (size_t i = 0; i < count; ++i) {
    if (patches[i].offset >= size || movie[patches[i].offset] != patches[i].before) return false;
  }
  for (size_t i = 0; i < count; ++i) movie[patches[i].offset] = patches[i].after;
  return true;
}

bool PatchOnlineMenuMovie(uint8_t* movie, size_t size) {
  if (!movie || size != kOnlineMenuMovieBytes || Fnv1a64(movie, size) != kOnlineMenuMovieHash) return false;
  return ApplyMovieBytePatches(movie, size, kOnlineMenuPatches, sizeof kOnlineMenuPatches / sizeof kOnlineMenuPatches[0]);
}

}  // namespace tmnt
