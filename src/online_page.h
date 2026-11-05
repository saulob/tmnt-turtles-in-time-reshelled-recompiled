#pragma once

#include <string>
#include <utility>
#include <vector>

#include "connection_test_gate.h"
#include "friend_code_field.h"
#include "clipboard.h"
#include "online/online_service.h"
#include "trg/launcher.h"
#include "trg/settings.h"

namespace tmnt {

// Sections of the Online page, in order: with Online play off only its switch; friend requests and party invites come
// first.
enum class OnlineSection {
  kOnlinePlay, kFriendRequests, kPartyInvites, kOnlineName, kFriendCode, kConnectionTest, kPingDisplay, kParty, kFriends
};
inline std::vector<OnlineSection> OnlinePageSections(bool online_on, size_t friend_requests, size_t party_invites = 0) {
  if (!online_on) return {OnlineSection::kOnlinePlay};
  std::vector<OnlineSection> sections{OnlineSection::kOnlinePlay};
  if (friend_requests > 0) sections.push_back(OnlineSection::kFriendRequests);
  if (party_invites > 0) sections.push_back(OnlineSection::kPartyInvites);
  for (const OnlineSection section : {OnlineSection::kOnlineName, OnlineSection::kFriendCode,
                                      OnlineSection::kConnectionTest, OnlineSection::kPingDisplay,
                                      OnlineSection::kParty, OnlineSection::kFriends}) {
    sections.push_back(section);
  }
  return sections;
}

// The launcher's Online page. Its service keeps running on every launcher frame, not only while the page is open.
class OnlinePage {
 public:
  // Applies the edited settings and returns what happened since the last frame, for the launcher's status line.
  std::vector<std::string> Tick(trg::SettingsStore& settings);
  // Online play is on and no Connection test has finished for its UDP configuration, or one is running.
  bool PlayNeedsTest(const trg::SettingsStore& settings) const;
  // Play was refused for the missing Connection test: its block is shown, focused and briefly emphasized.
  void RequestConnectionTestAttention() { attention_.Request(); }
  // Friend requests are waiting: the sidebar's attention dot.
  bool NeedsAttention() const;
  void Draw(trg::Ui& ui);
  // The launcher window, which owns what Copy puts on the clipboard.
  void SetOwnerWindow(void* window) { owner_window_ = window; }
  // Writes a finished Connection test to the player file at once.
  void SetSaveNow(SaveSettingNow save_now) { save_now_ = std::move(save_now); }

 private:
  void DrawFriendRequests(trg::Ui& ui, const online::Snapshot& state);
  void DrawPartyInvites(trg::Ui& ui, const online::Snapshot& state, bool usable);
  void DrawParty(trg::Ui& ui, const online::Snapshot& state, bool usable);
  void DrawFriendCode(trg::Ui& ui, const online::Snapshot& state);
  void DrawConnectionTest(trg::Ui& ui, const online::Snapshot& state);
  void DrawConnectionTestBody(trg::Ui& ui, const online::Snapshot& state, const ConnectionTestView& view,
                              bool focus_run_test);
  void DrawFriends(trg::Ui& ui, const online::Snapshot& state);
  std::string TestConfiguration() const;

  void* owner_window_ = nullptr;
  SaveSettingNow save_now_;
  ConnectionTestAttention attention_;
  int recorded_tests_ = 0;
  mutable double network_checked_at_ = -1.0;
  mutable std::string network_;
  char add_code_[kFriendCodeFieldSize] = {};
  FriendCodeField add_field_{[this](const FieldLog& log) { return ReadClipboardText(owner_window_, log); }};
};

}  // namespace tmnt
