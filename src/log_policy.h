#pragma once

#include <string_view>

namespace tmnt {

enum class LogSeverity { kBelowInfo, kInfo, kWarningOrAbove };

// Informational lines kept with Detailed logging off: what identifies a session and what happened to it.
inline constexpr std::string_view kNormalLogPrefixes[] = {
    "Turtles in Time Re-Shelled port ",
    "Build:",
    "Language:",
    "Launcher Display:",
    "Launcher Gameplay:",
    "Launcher Graphics:",
    "Launcher shown",
    "Runtime ",
    "User data:",
    "Player settings:",
    "Repaired ",
    "Removed the retired ",
    "Game files:",
    "Game files installed",
    "Game install cancelled",
    "Installing game files",
    "Graphics card:",
    "DXGI adapter:",
    "Vulkan device:",
    "Vulkan instance API version",
    "SDL video driver:",
    "Scheduling:",
    "Window mode:",
    "Updater:",
    "DLC installed",
    "DLC install cancelled",
    "DLC removed",
    "Installing DLC",
    "Removing DLC",
    "online ",
    "party ",
    "room ",
    "transport ",
    "relay ",
    "peer left",
    "direct path not established",
    "udp rendezvous failed",
    "udp socket ",
    "player_match ",
    "invite delivered",
    "accepted invite dropped",
    "game left ",
    "Windows crash reporting enabled for current log.",
    "Execution complete: clean exit",
    "Window closing",
    "Title terminated",
    "Launcher closed",
    "Restarted in a new window",
};

// Warnings and errors always stay, crash reports among them; everything else stays only when it is listed.
inline bool KeepWithoutDetailedLogging(LogSeverity severity, std::string_view message) {
  if (severity == LogSeverity::kWarningOrAbove) {
    return true;
  }
  if (severity == LogSeverity::kBelowInfo) {
    return false;
  }
  for (const std::string_view prefix : kNormalLogPrefixes) {
    if (message.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

}  // namespace tmnt
