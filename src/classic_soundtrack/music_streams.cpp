#include "classic_soundtrack/music_streams.h"

#include <algorithm>

namespace tmnt::classic {
namespace {

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;

std::uint64_t Mix(std::uint64_t value, std::uint8_t byte) {
  return (value ^ byte) * kFnvPrime;
}

}  // namespace

std::uint64_t XmaStreamFingerprint(const std::uint8_t* data, std::uint32_t bytes) {
  std::uint64_t value = kFnvOffset;
  for (int shift = 0; shift < 32; shift += 8) {
    value = Mix(value, std::uint8_t(bytes >> shift));
  }
  const std::uint32_t hashed = data ? std::min(bytes, kFingerprintBytes) : 0;
  for (std::uint32_t i = 0; i < hashed; ++i) {
    value = Mix(value, data[i]);
  }
  return value;
}

const MusicStream* FindMusicStream(std::span<const MusicStream> streams, std::uint64_t fingerprint, std::uint32_t bytes) {
  for (const MusicStream& stream : streams) {
    if (stream.fingerprint == fingerprint && stream.bytes == bytes) {
      return &stream;
    }
  }
  return nullptr;
}

}  // namespace tmnt::classic
