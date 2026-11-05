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

}  // namespace tmnt::live
