#pragma once

#include <cstdint>

namespace tmnt {

inline constexpr char kUnlockAllStagesSetting[] = "tmnt_unlock_all_stages";

inline constexpr uint32_t kStageSelectInitEvent = 166;  // UI_FIRE_REC_STAGE_SEL_INIT
inline constexpr uint32_t kStageSelectTrackOffset = 4;  // its track_no, var.i32_2
inline constexpr int kQuickPlayStageCount = 13;         // MAX_WORLD_ORDER

// Each time the stage select opens, the game queues an unlock message for every stage the profile completed and never
// a lock message. Unlocking all is queued after the game's own messages; going back to the real progression queues a
// lock for every stage before them, so the game's unlocks rebuild exactly what the profile holds. Nothing is stored.
class QuickPlayUnlock {
 public:
  struct Plan {
    bool lock_all_first = false;
    bool unlock_all_after = false;
  };

  Plan Open(uint32_t event, bool enabled, bool online) {
    if (event != kStageSelectInitEvent) {
      return {};
    }
    Plan plan;
    plan.unlock_all_after = enabled && !online;
    plan.lock_all_first = showing_all_ && !plan.unlock_all_after;
    showing_all_ = plan.unlock_all_after;
    return plan;
  }

  bool showing_all() const { return showing_all_; }

 private:
  bool showing_all_ = false;
};

}  // namespace tmnt
