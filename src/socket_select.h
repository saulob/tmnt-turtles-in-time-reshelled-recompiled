#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace tmnt {

struct FdSets {
  std::vector<uint32_t> read;
  std::vector<uint32_t> write;
  std::vector<uint32_t> except;
  bool empty() const { return read.empty() && write.empty() && except.empty(); }
};

struct SelectTimeout {
  bool infinite = false;
  uint32_t milliseconds = 0;
};

struct SelectOutcome {
  FdSets ready;
  int count = 0;
};

// select over a mix of shim sockets (answered from the shim) and real sockets (answered by `real_select`, which gets
// the real handles and returns how many are ready, or -1). Real handles are polled with a zero timeout so a ready shim
// socket is never delayed; with nothing ready the caller's timeout is honoured by repeating the poll. A shim-only call,
// or an infinite wait with nothing ready, never waits: shim data arrives from the network pump, which a blocked game
// thread would starve.
inline SelectOutcome SelectMixed(const FdSets& requested, const std::function<bool(uint32_t)>& owned,
                                 const std::function<bool(uint32_t)>& readable,
                                 const std::function<int(FdSets&)>& real_select, SelectTimeout timeout,
                                 const std::function<uint32_t()>& now_milliseconds,
                                 const std::function<void()>& pause) {
  FdSets shim_read, shim_write, real;
  for (const uint32_t handle : requested.read) (owned(handle) ? shim_read : real).read.push_back(handle);
  for (const uint32_t handle : requested.write) (owned(handle) ? shim_write : real).write.push_back(handle);
  for (const uint32_t handle : requested.except) {
    if (!owned(handle)) real.except.push_back(handle);
  }
  const uint32_t started = now_milliseconds();
  for (;;) {
    SelectOutcome outcome;
    for (const uint32_t handle : shim_read.read) {
      if (readable(handle)) outcome.ready.read.push_back(handle);
    }
    outcome.ready.write = shim_write.write;
    outcome.count = int(outcome.ready.read.size() + outcome.ready.write.size());
    if (!real.empty()) {
      FdSets polled = real;
      const int ready = real_select(polled);
      if (ready < 0) return {{}, -1};
      outcome.ready.read.insert(outcome.ready.read.end(), polled.read.begin(), polled.read.end());
      outcome.ready.write.insert(outcome.ready.write.end(), polled.write.begin(), polled.write.end());
      outcome.ready.except = polled.except;
      outcome.count += ready;
    }
    if (outcome.count > 0) return outcome;
    if (real.empty() || timeout.infinite || now_milliseconds() - started >= timeout.milliseconds) return {};
    pause();
  }
}

inline constexpr uint32_t kInfiniteWait = 0xFFFFFFFF;
inline constexpr uint32_t kWaitSliceMilliseconds = 5;

// The next short wait of a wait that is cut into slices: never longer than what is left, and none left once
// `elapsed_milliseconds` has reached a finite wait. The first slice of a zero wait is zero, so it still polls once.
inline uint32_t NextWaitSlice(uint32_t wait_milliseconds, uint32_t elapsed_milliseconds) {
  if (wait_milliseconds == kInfiniteWait) return kWaitSliceMilliseconds;
  const uint32_t left = wait_milliseconds > elapsed_milliseconds ? wait_milliseconds - elapsed_milliseconds : 0;
  return left < kWaitSliceMilliseconds ? left : kWaitSliceMilliseconds;
}

inline bool WaitExpired(uint32_t wait_milliseconds, uint32_t elapsed_milliseconds, bool first_slice) {
  return !first_slice && wait_milliseconds != kInfiniteWait && elapsed_milliseconds >= wait_milliseconds;
}

}  // namespace tmnt
