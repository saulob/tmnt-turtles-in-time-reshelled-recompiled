#pragma once

#include <mutex>

#include "classic_soundtrack/activation.h"
#include "localization/ui_text.h"

namespace tmnt {

enum class SoundtrackNotice { kNone, kOn, kOff, kUnavailable };

inline constexpr double kStartupNoticeSeconds = 5.0;
inline constexpr double kToggleNoticeSeconds = 3.0;

inline constexpr const char* SoundtrackNoticeText(SoundtrackNotice notice) {
  switch (notice) {
    case SoundtrackNotice::kOn: return "SNES soundtrack: On";
    case SoundtrackNotice::kOff: return "SNES soundtrack: Off";
    case SoundtrackNotice::kUnavailable: return "SNES soundtrack: Unavailable";
    case SoundtrackNotice::kNone: break;
  }
  return "";
}

inline const char* LocalizedSoundtrackNoticeText(SoundtrackNotice notice) {
  return notice == SoundtrackNotice::kNone ? "" : Tr(SoundtrackNoticeText(notice));
}

inline constexpr SoundtrackNotice NoticeForToggle(classic::RuntimeToggle toggle) {
  switch (toggle) {
    case classic::RuntimeToggle::kOn: return SoundtrackNotice::kOn;
    case classic::RuntimeToggle::kOff: return SoundtrackNotice::kOff;
    case classic::RuntimeToggle::kUnavailable: break;
  }
  return SoundtrackNotice::kUnavailable;
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
