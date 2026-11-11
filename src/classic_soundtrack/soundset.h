#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tmnt::classic {

inline constexpr size_t kAramSize = 0x10000;
inline constexpr size_t kSongCount = 38;
inline constexpr size_t kPackCount = 12;
inline constexpr size_t kStageCount = 12;
inline constexpr std::uint16_t kNoId = 0xFFFF;

inline constexpr std::uint16_t kDriverAddress = 0x0400;
inline constexpr std::uint16_t kSongAddress = 0x3000;
inline constexpr size_t kSongRegionSize = 0xE00;
inline constexpr std::uint16_t kPackSamplesAddress = 0x3E00;
inline constexpr size_t kPackSamplesLimit = 0x2300;
inline constexpr std::uint16_t kDirectoryAddress = 0xFE00;
inline constexpr size_t kDirectoryLimit = 0x180;

inline constexpr std::uint8_t kSongCommand6Track = 0x80;
inline constexpr std::uint8_t kSongCommand8Track = 0xA0;

inline constexpr std::uint16_t kSoundSetFormatVersion = 1;

struct AramBlock {
  std::uint16_t dest = 0;
  std::vector<std::uint8_t> data;
};

struct StagePack {
  std::uint8_t command = 0;
  std::vector<AramBlock> blocks;  // samples at 0x3E00, then the sample directory entries at 0xFE00
};

struct Song {
  std::uint8_t command = 0;
  std::uint16_t pack_id = kNoId;
  std::vector<std::uint8_t> data;  // loaded at kSongAddress
};

struct SoundSet {
  std::vector<AramBlock> boot;
  std::vector<StagePack> packs;
  std::vector<Song> songs;
  std::vector<std::uint16_t> stage_songs;  // song id per stage index, kNoId when none
};

enum class SoundSetError {
  kOk,
  kBadMagic,
  kUnsupportedVersion,
  kTruncated,
  kBadSection,
  kBadCount,
  kBadBlock,
  kBadSong,
  kBadBrr,
};

const char* SoundSetErrorText(SoundSetError error);

SoundSetError ValidateSoundSet(const SoundSet& set);

std::vector<std::uint8_t> SerializeSoundSet(const SoundSet& set);
SoundSetError DeserializeSoundSet(std::span<const std::uint8_t> bytes, SoundSet& set);

// Independent of the ROM file layout: the driver image plus every pack and song with its command.
std::string SoundtrackFingerprint(const SoundSet& set);

// The 64 KiB ARAM after the boot upload, then optionally a pack and a song on top.
std::vector<std::uint8_t> BuildAramImage(const SoundSet& set, int pack_id = -1, int song_id = -1);
bool WriteAramImage(const SoundSet& set, int pack_id, int song_id, std::span<std::uint8_t> aram);

}  // namespace tmnt::classic
