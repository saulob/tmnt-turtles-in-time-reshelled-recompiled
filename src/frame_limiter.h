#pragma once

#include <chrono>
#include <optional>

namespace tmnt {

using LimiterClock = std::chrono::steady_clock;

inline LimiterClock::duration FramePeriod(double rate_hz) {
  if (!(rate_hz > 0.0)) {
    return LimiterClock::duration::zero();
  }
  return std::chrono::duration_cast<LimiterClock::duration>(std::chrono::duration<double>(1.0 / rate_hz));
}

// The schedule of the guest frame limiter. Next() gives the time the caller has to wait for, if any.
// The phase is kept across frames, so a frame that missed its slot is not followed by a shorter one.
class FrameLimiter {
 public:
  std::optional<LimiterClock::time_point> Next(double rate_hz, LimiterClock::time_point now) {
    const auto period = FramePeriod(rate_hz);
    if (period == LimiterClock::duration::zero()) {
      armed_ = false;
      return std::nullopt;
    }
    if (!armed_ || rate_hz != rate_hz_) {
      armed_ = true;
      rate_hz_ = rate_hz;
      target_ = now + period;
      return std::nullopt;
    }
    const auto deadline = target_;
    target_ = now - deadline > period ? now + period : deadline + period;
    if (deadline <= now) {
      return std::nullopt;
    }
    return deadline;
  }

 private:
  bool armed_ = false;
  double rate_hz_ = 0.0;
  LimiterClock::time_point target_;
};

// Blocks the calling thread until the deadline: a high-resolution timer for most of it, then a short spin.
void WaitUntil(LimiterClock::time_point deadline);

// The refresh rate of the guest video mode, which is also the rate the runtime's vblank timer runs at.
double GuestRefreshRate();

// Called by the swap hook before the game's swap: holds the guest to one frame per vertical blank, as
// the guest's blocking present does on the console.
void PaceGuestFrame();

}  // namespace tmnt
