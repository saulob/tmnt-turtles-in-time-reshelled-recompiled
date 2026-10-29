// Guest frame limiter: one wait per guest frame, at the refresh rate of the guest video mode, with the
// phase kept across frames so a late frame is not followed by a burst of short ones. Also checks that the
// swap hook is what paces the guest.
// Run: tmnt_frame_limiter_tests

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "frame_limiter.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

using tmnt::FrameLimiter;
using tmnt::LimiterClock;

LimiterClock::time_point At(int milliseconds) {
  return LimiterClock::time_point{} + std::chrono::milliseconds(milliseconds);
}

int64_t Microseconds(tmnt::LimiterClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

void TestPeriod() {
  std::printf("period of a rate\n");
  CHECK(tmnt::FramePeriod(0.0) == LimiterClock::duration::zero());
  CHECK(tmnt::FramePeriod(-30.0) == LimiterClock::duration::zero());
  const int64_t at_60 = Microseconds(tmnt::FramePeriod(60.0));
  const int64_t at_30 = Microseconds(tmnt::FramePeriod(30.0));
  CHECK(at_60 >= 16600 && at_60 <= 16700);
  CHECK(at_30 >= 33300 && at_30 <= 33400);
}

void TestSteadyState() {
  std::printf("one wait per frame, one period apart\n");
  const auto period = tmnt::FramePeriod(60.0);
  FrameLimiter limiter;
  // The first frame only starts the schedule, so the game is never held back on its first swap.
  CHECK(!limiter.Next(60.0, At(0)).has_value());
  std::optional<LimiterClock::time_point> deadline = limiter.Next(60.0, At(1));
  CHECK(deadline.has_value());
  CHECK(*deadline == At(0) + period);

  // Later frames wait for the same schedule, and the deadlines never drift.
  for (int frame = 2; frame < 20; ++frame) {
    const auto now = *deadline + std::chrono::milliseconds(2);
    deadline = limiter.Next(60.0, now);
    CHECK(deadline.has_value());
    CHECK(*deadline == At(0) + period * frame);
  }
}

void TestMissedSlot() {
  std::printf("a frame that missed its slot is not followed by a shorter one\n");
  const auto period = tmnt::FramePeriod(60.0);
  FrameLimiter limiter;
  CHECK(!limiter.Next(60.0, At(0)).has_value());
  CHECK(limiter.Next(60.0, At(1)).has_value());

  // The frame at 41 ms is past the slot at 2 periods: no wait for it, and the next slot is the one it
  // already missed, one full period after this frame rather than in the past.
  CHECK(!limiter.Next(60.0, At(41)).has_value());
  const std::optional<LimiterClock::time_point> deadline = limiter.Next(60.0, At(42));
  CHECK(deadline.has_value());
  CHECK(*deadline == At(0) + period * 3);
}

void TestRateChange() {
  std::printf("a change of rate re-arms the schedule\n");
  const auto period_30 = tmnt::FramePeriod(30.0);
  FrameLimiter limiter;
  CHECK(!limiter.Next(60.0, At(0)).has_value());
  CHECK(limiter.Next(60.0, At(1)).has_value());

  // Time spent at the other rate is neither waited for nor caught up with.
  CHECK(!limiter.Next(30.0, At(5)).has_value());
  const std::optional<LimiterClock::time_point> deadline = limiter.Next(30.0, At(6));
  CHECK(deadline.has_value());
  CHECK(*deadline == At(5) + period_30);

  // No rate at all means no waits until a rate is set again.
  CHECK(!limiter.Next(0.0, At(7)).has_value());
  CHECK(!limiter.Next(0.0, At(8)).has_value());
  CHECK(!limiter.Next(60.0, At(9)).has_value());
  CHECK(limiter.Next(60.0, At(10)).has_value());
}

void TestSwapHook() {
  std::printf("the swap hook paces the guest before the game's swap\n");
  const std::string window_host = ReadSource("window_host.cpp");
  const std::size_t pace = window_host.find("tmnt::PaceGuestFrame();");
  const std::size_t swap = window_host.find("HostToGuestFunction<rex::kernel::xboxkrnl::VdSwap_entry>");
  CHECK(pace != std::string::npos);
  CHECK(swap != std::string::npos);
  CHECK(pace < swap);
  // One rate for the limiter and the vblank timer both.
  CHECK(Has(ReadSource("frame_limiter.cpp"), "REXCVAR_GET(video_mode_refresh_rate)"));

  const std::string cmake = ReadSource("../CMakeLists.txt");
  CHECK(Has(cmake, "    src/frame_limiter.cpp\n") || Has(cmake, "    src/frame_limiter.cpp\r\n"));
  CHECK(Has(cmake, "    src/frame_wait.cpp\n") || Has(cmake, "    src/frame_wait.cpp\r\n"));
}

}  // namespace

int main() {
  TestPeriod();
  TestSteadyState();
  TestMissedSlot();
  TestRateChange();
  TestSwapHook();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
