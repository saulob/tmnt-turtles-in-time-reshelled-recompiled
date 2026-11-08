#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace tmnt {

// present_letterbox: the presenter always letterboxes the chosen format. tmnt_snes_gameplay: the SNES features no longer
// have a master switch and each stands alone.
inline constexpr std::array<std::string_view, 2> kRetiredPlayerSettings{"present_letterbox", "tmnt_snes_gameplay"};

inline bool IsSettingLine(std::string_view line, std::string_view key) {
  const std::size_t first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return false;
  }
  std::string_view rest = line.substr(first);
  if (!rest.starts_with(key)) {
    return false;
  }
  rest.remove_prefix(key.size());
  const std::size_t next = rest.find_first_not_of(" \t");
  return next != std::string_view::npos && rest[next] == '=';
}

// Drops the lines that set key. Every other byte is kept, including CR and a missing final newline.
inline std::string RemoveSettingLines(std::string_view text, std::string_view key) {
  std::string kept;
  kept.reserve(text.size());
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::size_t length = end == std::string_view::npos ? text.size() : end + 1;
    const std::string_view line = text.substr(0, length);
    if (!IsSettingLine(line, key)) {
      kept.append(line);
    }
    text.remove_prefix(length);
  }
  return kept;
}

}  // namespace tmnt
