#pragma once

#include <mutex>

#include "cheats.h"
#include "localization/ui_text.h"

namespace tmnt {

enum class SoundtrackNotice { kNone, kOn, kCheatsUnavailable };

inline constexpr double kStartupNoticeSeconds = 5.0;
inline constexpr double kHotkeyNoticeSeconds = 3.0;

inline constexpr const char* SoundtrackNoticeText(SoundtrackNotice notice) {
  switch (notice) {
    case SoundtrackNotice::kOn: return "SNES soundtrack: On";
    case SoundtrackNotice::kCheatsUnavailable: return cheats::kOnlineLockoutText;
    case SoundtrackNotice::kNone: break;
  }
  return "";
}

inline const char* LocalizedSoundtrackNoticeText(SoundtrackNotice notice) {
  return notice == SoundtrackNotice::kNone ? "" : Tr(SoundtrackNoticeText(notice));
}

// One short message at a time. A new message replaces the old one, and its time runs from the first frame that asks
// for it, so a message requested before the window exists still shows for its whole length.
class SoundtrackNoticeState {
 public:
  void Show(SoundtrackNotice notice, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    notice_ = notice;
    seconds_ = seconds;
    started_ = false;
  }

  SoundtrackNotice Current(double now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (notice_ == SoundtrackNotice::kNone) {
      return SoundtrackNotice::kNone;
    }
    if (!started_) {
      started_ = true;
      start_ = now;
    }
    if (now - start_ >= seconds_) {
      notice_ = SoundtrackNotice::kNone;
    }
    return notice_;
  }

 private:
  std::mutex mutex_;
  SoundtrackNotice notice_ = SoundtrackNotice::kNone;
  double seconds_ = 0;
  double start_ = 0;
  bool started_ = false;
};

}  // namespace tmnt
