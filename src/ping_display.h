#pragma once

#include <optional>
#include <string>

#include "online/online_service.h"

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace tmnt {

// "Ping: 32 ms" (the worst round trip among the party's connected players), or "Ping: --" while none has been measured.
inline std::string PingText(const std::optional<int>& rtt_ms) {
  return rtt_ms ? "Ping: " + std::to_string(*rtt_ms) + " ms" : "Ping: --";
}

// Shown with Online play on and the display switched on; without a connected peer it reads "Ping: --".
constexpr bool PingVisible(bool online_enabled, bool display_on) { return online_enabled && display_on; }

inline std::optional<int> MeasuredRtt(online::NetplayLink link, const std::optional<int>& rtt_ms) {
  return link >= online::NetplayLink::kConnected ? rtt_ms : std::nullopt;
}

inline constexpr char kPingDisplayKey[] = "F3";

// Adds the in-game Ping panel and its F3 toggle.
void CreatePingDisplay(rex::ui::ImGuiDrawer* drawer);

}  // namespace tmnt
