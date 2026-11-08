#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tmnt::classic {

struct KnownRom {
  std::string_view sha256;
  std::string_view region;
};

inline constexpr std::array<KnownRom, 3> kKnownRoms = {{
    {"5b82cdd6f2da56f43680d6a5021faebe2e06036d30602c1a7917aa414cf8b5f4", "USA"},
    {"0bf60b1feab9379027487f4656688c6b1f2993a462aa0663c71af220a2afde21", "Europe"},
    {"f1d643b3ea19dac6cff63b231a5aefa2f45335fcac86fcb590e008d94828b4cb", "Australia"},
}};

inline constexpr std::string_view kKnownSoundtrackFingerprint =
    "149c010ed2fe35edad66983fd7fcffa73e596a5dd06b9e45512f15fc94d5f7e5";

inline constexpr std::string_view kCompatibleRegion = "Compatible";

// Names from the ROM's own sound test; slots without one stay unnamed.
inline constexpr std::array<std::string_view, 38> kSongLabels = {{
    "ROOF", "BATTLE", "CLEAR", "GAME OVER", "DOWNTOWN", "SELECT", "OPEN", "TRAINING", "BAD NEWS", "SEWERAGE",
    "", "DINOSAUR", "PIRATE", "TRAIN", "HIGHWAY", "STAR BASE", "", "LAST BOSS", "WARP3", "",
    "CONTINUE", "GOOD NEWS", "STAFF", "FORTRESS", "COURSE", "ATTACK", "WARP2", "ELEVETOR", "MEMORY", "ESCAPE",
    "GOOD", "WINNER", "", "", "", "", "ARRIVE", "HURRY",
}};

inline std::string_view SongLabel(size_t song_id) {
  return song_id < kSongLabels.size() ? kSongLabels[song_id] : std::string_view();
}

inline std::string_view KnownRomRegion(std::string_view sha256) {
  for (const KnownRom& rom : kKnownRoms) {
    if (rom.sha256 == sha256) {
      return rom.region;
    }
  }
  return {};
}

}  // namespace tmnt::classic
