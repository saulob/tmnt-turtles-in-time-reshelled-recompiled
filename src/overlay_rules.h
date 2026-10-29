#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "achievement_alias.h"

namespace tmnt {

constexpr float kOverlayViewportFraction = 0.7f;
constexpr float kOverlayViewportMargin = 16.0f;

// Window size along one axis: about 70% of the viewport, clamped to [min_size, max_size] and kept inside it.
inline float FitWindowExtent(float viewport, float min_size, float max_size) {
  const float wanted = std::clamp(viewport * kOverlayViewportFraction, min_size, max_size);
  return std::max(0.0f, std::min(wanted, viewport - 2.0f * kOverlayViewportMargin));
}

struct AchievementSummary {
  int unlocked = 0;
  int total = 0;
  int earned_gamerscore = 0;
  int total_gamerscore = 0;
};

// Totals over the achievements the store lists; `unlocked_flags[i]` says whether entry i is earned.
inline AchievementSummary Summarize(const std::vector<uint32_t>& gamerscores, const std::vector<bool>& unlocked_flags) {
  AchievementSummary summary;
  summary.total = static_cast<int>(gamerscores.size());
  for (size_t i = 0; i < gamerscores.size(); ++i) {
    summary.total_gamerscore += static_cast<int>(gamerscores[i]);
    if (i < unlocked_flags.size() && unlocked_flags[i]) {
      ++summary.unlocked;
      summary.earned_gamerscore += static_cast<int>(gamerscores[i]);
    }
  }
  return summary;
}

// A button that is never reported released (a controller unplugged as the dialog closes) cannot block the game forever.
constexpr double kReleaseTimeoutSeconds = 1.0;

// Whether the modal may release the game's input: nothing it received is still held, or the wait ran out.
inline bool InputReleased(bool anything_held, double waited_seconds, double timeout_seconds) {
  return !anything_held || waited_seconds >= timeout_seconds;
}

// Closing a modal: asking to close (B, Escape, F7, OK) starts a wait, and the game gets its input back only after
// what was pressed in the dialog is released, so the closing press never also reaches the game's menu.
class CloseGate {
 public:
  void Request() { requested_ = true; }
  bool requested() const { return requested_; }

  // Once per frame. True when the dialog may go and the game may read its input again.
  bool Step(bool anything_held, double now) {
    if (!requested_) return false;
    if (since_ < 0.0) since_ = now;
    return InputReleased(anything_held, now - since_, kReleaseTimeoutSeconds);
  }

 private:
  bool requested_ = false;
  double since_ = -1.0;
};

}  // namespace tmnt
