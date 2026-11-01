#pragma once

#include <string>
#include <string_view>

namespace tmnt {

// Lets a repeated line through once, then reports how many identical lines were dropped when the text changes.
class RepeatFilter {
 public:
  // Returns the number of dropped repeats to report before `text`, or -1 to drop `text` itself.
  int Next(std::string_view text) {
    if (text == last_) {
      ++dropped_;
      return -1;
    }
    last_ = std::string(text);
    const int dropped = dropped_;
    dropped_ = 0;
    return dropped;
  }

 private:
  std::string last_;
  int dropped_ = 0;
};

// At most `per_second` lines in any one-second window; the rest are counted.
class LineBudget {
 public:
  explicit LineBudget(unsigned per_second) : per_second_(per_second) {}
  bool Allow(double now_s) {
    if (now_s - window_s_ >= 1.0) {
      window_s_ = now_s;
      used_ = 0;
    }
    if (used_ < per_second_) {
      ++used_;
      return true;
    }
    ++dropped_;
    return false;
  }
  unsigned TakeDropped() {
    const unsigned dropped = dropped_;
    dropped_ = 0;
    return dropped;
  }

 private:
  unsigned per_second_;
  double window_s_ = -1e9;
  unsigned used_ = 0;
  unsigned dropped_ = 0;
};

// The script tracks that carry the online session and lobby flow.
inline bool IsSessionTrack(std::string_view name) {
  return name.find("SessionManager") != std::string_view::npos || name.find("FireCommunicator") != std::string_view::npos;
}

}  // namespace tmnt
