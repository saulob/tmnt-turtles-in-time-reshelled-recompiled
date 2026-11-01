#pragma once

#include <cmath>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "netplay_setting.h"
#include "online/protocol.h"
#include "online_setting.h"
#include "trg/settings.h"

namespace tmnt {

// What the last usable Connection test proved, written with Online play on; empty: none since Online play was turned
// on, or the last test was not usable.
inline constexpr char kConnectionTestSetting[] = "tmnt_online_connection_test";

inline constexpr char kConnectionTestRequired[] = "Run the Connection test on the Online page before playing online.";
inline constexpr char kConnectionTestRequiredLabel[] = "Connection test required";

// The gameplay ports in the order the online socket tries them (every further instance on this PC gets the next one).
inline std::string OnlineUdpConfiguration() {
  return "udp " + std::to_string(kGameplayPorts[0]) + "-" + std::to_string(kGameplayPorts[std::size(kGameplayPorts) - 1]);
}

// The /24 prefixes of this PC's private IPv4 addresses; another network gives another value.
inline std::string NetworkFingerprint(const std::vector<std::string>& local_addresses) {
  std::set<std::string> prefixes;
  for (const std::string& address : local_addresses) {
    const size_t last_dot = address.rfind('.');
    if (last_dot != std::string::npos && address.compare(0, 4, "127.") != 0) prefixes.insert(address.substr(0, last_dot));
  }
  std::string text;
  for (const std::string& prefix : prefixes) text += (text.empty() ? "" : ",") + prefix;
  return text;
}

// Everything a successful test depends on: the gameplay ports, the online service and its protocol, and the network.
inline std::string OnlineTestConfiguration(const std::string& service_url, const std::string& network) {
  return OnlineUdpConfiguration() + "; service " + service_url + "; protocol " + std::to_string(online::kLobbyProtocol) +
         "; build " + std::to_string(online::kOnlineBuild) + "; network " + network;
}

// With Online play on, PLAY waits for a Connection test that was usable for this configuration, and while one runs.
inline bool ConnectionTestNeeded(const trg::SettingsStore& settings, bool testing,
                                 const std::string& configuration = OnlineUdpConfiguration()) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled)) return false;
  return testing || settings.Get(kConnectionTestSetting) != configuration;
}

using SaveSettingNow = std::function<bool(std::string_view key, const std::string& value)>;

// Written to the player file at once as well, so closing the launcher without Save keeps it. A test that was not
// usable clears the earlier one, so PLAY stays blocked. Nothing with Online play off: a test then proves nothing for the
// next time it is turned on.
inline bool RecordConnectionTest(trg::SettingsStore& settings, const SaveSettingNow& save_now, bool usable,
                                 const std::string& configuration = OnlineUdpConfiguration()) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled)) return false;
  const std::string value = usable ? configuration : std::string();
  settings.Set(kConnectionTestSetting, value);
  return !save_now || save_now(kConnectionTestSetting, value);
}

// Online play off forgets the test, pending until Save like the switch itself, so turning it on again asks for one.
inline void ForgetConnectionTestWhenOff(trg::SettingsStore& settings) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled) && !settings.Get(kConnectionTestSetting).empty()) {
    settings.Set(kConnectionTestSetting, "");
  }
}

// Under a running Connection test, whose socket may be the first use of the multiplayer port Windows Firewall asks
// about; nothing elsewhere.
inline const char* ConnectionTestRunningNote(bool windows) {
  return windows ? "If Windows asks whether to allow network access, choose Allow access so other players can "
                   "connect to you."
                 : nullptr;
}

// What the Connection test block of the Online page shows. A usable test ends the required look; a report that was not
// usable stays visible with the block still required.
struct ConnectionTestView {
  bool highlighted = false;     // the gold border around the whole block
  bool primary_button = false;  // Run test as the launcher's accent button
  bool required_text = false;
  bool windows_note = false;
};

inline ConnectionTestView DescribeConnectionTestView(bool online_on, bool needed, bool testing, bool on_windows) {
  ConnectionTestView view;
  view.windows_note = online_on && testing && on_windows;
  view.highlighted = online_on && needed;
  view.primary_button = view.highlighted && !testing;
  view.required_text = view.highlighted && !testing;
  return view;
}

// The short emphasis after Play was refused for the missing test: the block is scrolled into view and Run test focused
// once, and its border pulses for a moment. A new request restarts it; ordinary frames do not.
class ConnectionTestAttention {
 public:
  static constexpr double kSeconds = 2.5;
  static constexpr double kScrollSeconds = 0.4;  // the page may only be showing from the frame after the request
  static constexpr int kPulses = 3;

  struct Step {
    bool scroll_into_view = false;
    bool focus_run_test = false;
    float strength = 0.0f;  // 0: normal required border, 1: brightest
  };

  void Request() { pending_ = true; }

  Step Frame(double now, bool required) {
    Step step;
    if (!required) {
      pending_ = false;
      started_ = -1.0;
      return step;
    }
    if (pending_) {
      pending_ = false;
      started_ = now;
      step.focus_run_test = true;
    }
    if (started_ >= 0.0) {
      const double t = (now - started_) / kSeconds;
      if (t >= 1.0 || t < 0.0) {
        started_ = -1.0;
      } else {
        step.scroll_into_view = now - started_ < kScrollSeconds;
        step.strength = float((1.0 - t) * (0.5 + 0.5 * std::cos(t * kPulses * 6.283185307179586)));
      }
    }
    return step;
  }

  bool active() const { return pending_ || started_ >= 0.0; }

 private:
  bool pending_ = false;
  double started_ = -1.0;
};

// Play pressed: the launcher asks whether Play may start once per frame to draw its button, and once more for the press.
class PlayAttemptDetector {
 public:
  bool PressedThisCall(int frame) {
    const bool again = frame == frame_;
    frame_ = frame;
    return again;
  }

 private:
  int frame_ = -1;
};

#if defined(_WIN32)
inline constexpr bool kOnWindows = true;
#else
inline constexpr bool kOnWindows = false;
#endif

}  // namespace tmnt
