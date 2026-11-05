#pragma once

#include <functional>
#include <string>

namespace tmnt {

struct ClipboardText {
  enum class Kind { kText, kEmpty, kError };
  Kind kind = Kind::kEmpty;
  std::string text;  // UTF-8 clipboard text, or the error
};

using ClipboardLog = std::function<void(const std::string&)>;

// The system clipboard as plain text. `owner_window` is the native window that owns a write (HWND on Windows), or null.
// Failures go to `log`.
ClipboardText ReadClipboardText(void* owner_window, const ClipboardLog& log);
bool WriteClipboardText(void* owner_window, const std::string& text, const ClipboardLog& log);

}  // namespace tmnt
