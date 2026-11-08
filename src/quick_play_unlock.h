#pragma once

#include <cstdint>

namespace tmnt {

// DEBUG_SAULO: Quick Play lists a stage once the profile has completed it. In a RelWithDebInfo build the stage
// select shows every stage, the way the script's own DEBUG build does. Nothing is written to a profile or save.
#if defined(TMNT_DEBUG_QUICKPLAY_UNLOCK) && TMNT_DEBUG_QUICKPLAY_UNLOCK
inline constexpr bool kQuickPlayUnlockBuild = true;
#else
inline constexpr bool kQuickPlayUnlockBuild = false;
#endif

inline constexpr uint32_t kStageSelectInitEvent = 166;  // UI_FIRE_REC_STAGE_SEL_INIT
inline constexpr int kQuickPlayStageCount = 13;         // MAX_WORLD_ORDER

constexpr bool UnlocksAllQuickPlayStages(uint32_t event, bool unlock_build) {
  return unlock_build && event == kStageSelectInitEvent;
}

}  // namespace tmnt
