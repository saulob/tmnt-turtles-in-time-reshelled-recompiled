#include "frame_limiter.h"

#include <algorithm>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/ui/flags.h>

namespace tmnt {
namespace {

constexpr double kMinRefreshRate = 24.0;
constexpr double kMaxRefreshRate = 240.0;

FrameLimiter g_limiter;
std::mutex g_limiter_mutex;

}  // namespace

double GuestRefreshRate() {
  return std::clamp(REXCVAR_GET(video_mode_refresh_rate), kMinRefreshRate, kMaxRefreshRate);
}

void PaceGuestFrame() {
  std::optional<LimiterClock::time_point> deadline;
  {
    std::lock_guard lock(g_limiter_mutex);
    deadline = g_limiter.Next(GuestRefreshRate(), LimiterClock::now());
  }
  if (deadline) {
    WaitUntil(*deadline);
  }
}

}  // namespace tmnt
