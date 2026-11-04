#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tmnt {

// Rewrites `key = on` and `key = off` as `key = true` and `key = false`, the only
// bare values an unreleased development build wrote. Every other byte is kept.
std::string RepairLegacyToggles(std::string_view text, std::size_t& repaired);

// A TOML basic string.
std::string TomlString(std::string_view value);

// Quotes the bare text of `key = text`, as written by launchers that saved it unquoted; ReXGlue rejects the whole file
// for one such line. Every other byte is kept.
std::string RepairBareString(std::string_view text, std::string_view key, std::size_t& repaired);

// A TOML basic string.
std::string TomlString(std::string_view value);

// Quotes the bare text of `key = text`, as written by launchers that saved it unquoted; ReXGlue rejects the whole file
// for one such line. Every other byte is kept.
std::string RepairBareString(std::string_view text, std::string_view key, std::size_t& repaired);

}  // namespace tmnt
