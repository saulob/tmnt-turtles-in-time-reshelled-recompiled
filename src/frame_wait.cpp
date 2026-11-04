#include "frame_limiter.h"
#include "frame_trace.h"

#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace tmnt {
namespace {

using namespace std::chrono_literals;

// The timer wakes within this of the deadline; the rest is waited for by spinning.
constexpr auto kSpinWindow = 500us;

}  // namespace

#if defined(_WIN32)

namespace {

uint64_t Microseconds(const FILETIME& kernel, const FILETIME& user) {
  const auto ticks = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10;
}

}  // namespace

uint64_t ThreadCpuMicroseconds() {
  FILETIME created, ended, kernel, user;
  return GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user) ? Microseconds(kernel, user) : 0;
}

uint64_t ProcessCpuMicroseconds() {
  FILETIME created, ended, kernel, user;
  return GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user) ? Microseconds(kernel, user) : 0;
}

void WaitUntil(LimiterClock::time_point deadline) {
  thread_local const HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto coarse = deadline - LimiterClock::now() - kSpinWindow;
  if (coarse > LimiterClock::duration::zero()) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100;
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
      }
    } else {
      std::this_thread::sleep_for(coarse);
    }
  }
  while (LimiterClock::now() < deadline) {
    std::this_thread::yield();
  }
}

#else

uint64_t ThreadCpuMicroseconds() { return 0; }
uint64_t ProcessCpuMicroseconds() { return 0; }

void WaitUntil(LimiterClock::time_point deadline) {
  std::this_thread::sleep_until(deadline - kSpinWindow);
  while (LimiterClock::now() < deadline) {
    std::this_thread::yield();
  }
}

#endif

}  // namespace tmnt
