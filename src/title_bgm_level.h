#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tmnt {

inline constexpr int32_t kDefaultMusicLevel = 10;  // SND_LEVEL_DEFAULT, the level of a profile that saved none
inline constexpr size_t kSavedMusicLevelOffset = 76;  // SAV_SETTING_MUSIC
inline constexpr char kProfileSettingsFile[] = "63E83FFF";

// The level the game applies from this profile blob when Start is pressed.
inline int32_t SavedMusicLevel(std::span<const uint8_t> blob) {
  if (blob.size() < kSavedMusicLevelOffset + 4) return kDefaultMusicLevel;
  const uint8_t* at = blob.data() + kSavedMusicLevelOffset;
  const int32_t level = int32_t(uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | uint32_t(at[3]));
  return level >= 0 && level <= kDefaultMusicLevel ? level : kDefaultMusicLevel;
}

inline std::filesystem::path SavedSettingsPath(const std::filesystem::path& user_data_root, uint32_t title_id) {
  char title[9];
  std::snprintf(title, sizeof(title), "%08X", unsigned(title_id));
  return user_data_root / title / "profile" / "User" / kProfileSettingsFile;
}

inline int32_t ReadSavedMusicLevel(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  return SavedMusicLevel(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}

// What the first SoundBGM_Init stores in m_BGMVolume, from the value the exe published; nothing leaves the game's default.
inline std::optional<int32_t> TitleMusicSeed(std::string_view published) {
  int32_t level = 0;
  const char* end = published.data() + published.size();
  const auto parsed = std::from_chars(published.data(), end, level);
  if (published.empty() || parsed.ec != std::errc() || parsed.ptr != end) return std::nullopt;
  if (level < 0 || level > kDefaultMusicLevel) return std::nullopt;
  return level;
}

// Publishes the saved level for the TMNT_r hook. Read from the file before the guest starts: reading the profile through
// the runtime from a guest thread crashed.
void PublishTitleMusicLevel(const std::filesystem::path& user_data_root);

}  // namespace tmnt
