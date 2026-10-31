#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "live_stats.h"

namespace tmnt::live {

struct InstallParams {
  std::filesystem::path stats_file;
  LocalUser user;
};

// Called once, before the game starts.
void Install(InstallParams params);

// The best rating the local store holds for the user and leaderboard view, if any.
std::optional<int64_t> LocalRating(uint64_t xuid, uint32_t view);

}  // namespace tmnt::live
