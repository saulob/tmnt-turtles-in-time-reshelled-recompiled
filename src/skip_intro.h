#pragma once

#include <cstdint>
#include <string_view>

namespace tmnt {

inline constexpr uint32_t kLogoWorld = 0x374034E5;       // LOGO_WORLD
inline constexpr uint32_t kStartMenuWorld = 0x5040027C;  // START_MENU_WORLD

inline bool SkipIntroEnabled(std::string_view setting) { return setting == "true"; }

// Universe_Init alone sends the game to the Logo world, the head of the startup presentation.
inline uint32_t StartupWorldTarget(uint32_t next_world, bool skip_intro) {
  return skip_intro && next_world == kLogoWorld ? kStartMenuWorld : next_world;
}

}  // namespace tmnt
