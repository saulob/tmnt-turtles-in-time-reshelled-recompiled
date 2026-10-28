#include "legacy_settings.h"

#include <cctype>
#include <optional>

namespace tmnt {
namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kBlanks = " \t\r";

bool IsBareKey(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  for (char c : key) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

std::optional<std::string> RepairLine(std::string_view line) {
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos || line[first] == '#' || line[first] == '[') {
    return std::nullopt;
  }
  const size_t eq = line.find('=');
  if (eq == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view key = line.substr(first, eq - first);
  key = key.substr(0, key.find_last_not_of(" \t") + 1);
  if (!IsBareKey(key)) {
    return std::nullopt;
  }
  const size_t value_start = line.find_first_not_of(" \t", eq + 1);
  if (value_start == std::string_view::npos) {
    return std::nullopt;
  }
  size_t value_end = line.find_first_of(kBlanks, value_start);
  if (value_end == std::string_view::npos) {
    value_end = line.size();
  }
  const std::string_view value = line.substr(value_start, value_end - value_start);
  const std::string_view trailing = line.substr(value_end);
  if (trailing.find_first_not_of(kBlanks) != std::string_view::npos) {
    return std::nullopt;
  }
  if (value != "on" && value != "off") {
    return std::nullopt;
  }
  return std::string(line.substr(0, value_start)) + (value == "on" ? "true" : "false") + std::string(trailing);
}

}  // namespace

std::string RepairLegacyToggles(std::string_view text, std::size_t& repaired) {
  std::string out;
  out.reserve(text.size());
  if (text.starts_with(kBom)) {
    out += kBom;
    text.remove_prefix(kBom.size());
  }
  size_t start = 0;
  while (start < text.size()) {
    const size_t newline = text.find('\n', start);
    const bool has_newline = newline != std::string_view::npos;
    const size_t end = has_newline ? newline : text.size();
    const std::string_view line = text.substr(start, end - start);
    if (std::optional<std::string> fixed = RepairLine(line)) {
      out += *fixed;
      ++repaired;
    } else {
      out += line;
    }
    if (has_newline) {
      out += '\n';
    }
    start = has_newline ? newline + 1 : text.size();
  }
  return out;
}

}  // namespace tmnt
