#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tmnt {

// Rewrites `key = on` and `key = off` as `key = true` and `key = false`, the only
// bare values an unreleased development build wrote. Every other byte is kept.
std::string RepairLegacyToggles(std::string_view text, std::size_t& repaired);

}  // namespace tmnt
