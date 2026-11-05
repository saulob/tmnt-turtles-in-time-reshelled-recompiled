#include "online_page.h"

#include <cfloat>
#include <cstring>
#include <string>

#include <imgui.h>

#include "friend_code_field.h"
#include "localization/ui_text.h"
#include "online/friend_view.h"
#include "online/identity.h"
#include "online/local_addresses.h"
#include "online/protocol.h"
#include "online_runtime.h"
#include "online_setting.h"

namespace tmnt {
namespace {

constexpr char kFriendCodeHelp[] = "Share this code so another player can add you.";
constexpr char kTestHelp[] =
    "Checks that you can reach the online service and that other players can reach you. Takes a few seconds.";
constexpr char kUnreachable[] = "Cannot reach the online service right now. Run the Connection test for details.";
constexpr char kPartyHelp[] =
    "Start a party, invite up to three friends from the list below, and play together. Friends who accept join the "
    "same game.";
constexpr char kNoFriends[] = "No friends yet. Add someone with their friend code.";

int OnlineNameFilter(ImGuiInputTextCallbackData* data) {
  return data->EventChar < 0x80 && online::OnlineNameChar(char(data->EventChar)) ? 0 : 1;
}

// Typing and pasting keep only what the service accepts, so the field never shows a name other players will not see.
void OnlineNameRow(trg::Ui& ui) {
  ui.Row(kOnlineNameLabel, kOnlineNameHelp);
  const std::string saved = ui.settings().Get(kOnlineNameSetting);
  char text[online::kMaxOnlineNameChars + 1] = {};
  saved.copy(text, online::kMaxOnlineNameChars);
  ImGui::PushID(kOnlineNameSetting);
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputTextWithHint("##text", kDefaultOnlineNameValue, text, sizeof text,
                               ImGuiInputTextFlags_CallbackCharFilter, OnlineNameFilter)) {
    ui.settings().Set(kOnlineNameSetting, text);
  }
  ImGui::PopID();
  const std::string shown = online::CleanOnlineName(saved);
  if (!saved.empty() && shown != saved) ui.Help((Tr("Other players see ") + shown + ".").c_str());
}

bool AcceptButton(trg::Ui& ui, const char* id) {
  const trg::Theme& t = ui.theme();
  trg::PushActionButton(t, ui.scale());
  const float mark = ImGui::GetFontSize() * 0.8f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Button(id);
  const ImVec2 pad = ImGui::GetStyle().FramePadding;
  const ImVec2 origin(at.x + pad.x, at.y + pad.y + ImGui::GetFontSize() * 0.1f);
  const ImVec2 tick[3] = {ImVec2(origin.x + mark * 0.1f, origin.y + mark * 0.55f),
                          ImVec2(origin.x + mark * 0.4f, origin.y + mark * 0.85f),
                          ImVec2(origin.x + mark * 0.95f, origin.y + mark * 0.2f)};
  ImGui::GetWindowDrawList()->AddPolyline(tick, 3, ImGui::GetColorU32(t.on_action), ImDrawFlags_None, mark * 0.18f);
  trg::PopActionButton();
  return clicked;
}

// The name, then the status line in the row's help style under it. ui.Row has already moved to the control column, so
// the line is drawn back in the label column.
void FriendHeading(trg::Ui& ui, const std::string& name, const online::FriendRowView& view) {
  ui.Row(name.c_str());
  const bool in_table = ImGui::TableGetColumnCount() > 0;
  if (in_table) {
    const float below_name = ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y;
    ImGui::TableSetColumnIndex(0);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, below_name));
  }
  ImGui::PushFont(nullptr, ui.fonts().size * 0.84f);
  ImGui::TextColored(ui.theme().dim, "%s", TrMessage(view.status).c_str());
  if (view.incompatible) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ui.theme().dim, " \xC2\xB7 ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ui.theme().warn_dot, "%s", Tr("Game versions do not match"));
  }
  ImGui::PopFont();
  if (in_table) {
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
  }
}

}  // namespace

std::vector<std::string> OnlinePage::Tick(trg::SettingsStore& settings) {
  online::UserSettings edited;
  edited.enabled = settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
  edited.name = settings.Get(kOnlineNameSetting, kDefaultOnlineNameValue);
  std::vector<std::string> notices;
  TickOnline(edited);
  for (std::string& notice : Online().TakeNotices()) notices.push_back(std::move(notice));
  ForgetConnectionTestWhenOff(settings);
  const online::Snapshot state = Online().snapshot();
  if (state.tests_finished != recorded_tests_) {
    recorded_tests_ = state.tests_finished;
    const bool usable = state.test && state.test->overall_usable;
    if (RecordConnectionTest(settings, save_now_, usable, TestConfiguration())) {
      Online().LogLine(std::string("online connection test completion saved usable=") +
                       (usable ? "true" : "false"));
    }
  }
  return notices;
}

std::string OnlinePage::TestConfiguration() const {
  const double now = ImGui::GetTime();
  if (network_checked_at_ < 0.0 || now - network_checked_at_ > 10.0) {
    network_checked_at_ = now;
    network_ = NetworkFingerprint(online::LocalCandidates());
  }
  return OnlineTestConfiguration(Online().url(), network_);
}

bool OnlinePage::PlayNeedsTest(const trg::SettingsStore& settings) const {
  return ConnectionTestNeeded(settings, Online().snapshot().testing, TestConfiguration());
}

bool OnlinePage::NeedsAttention() const {
  const online::Snapshot state = Online().snapshot();
  return !state.incoming.empty() || !state.invites.empty();
}

void OnlinePage::Draw(trg::Ui& ui) {
  const bool on = ui.settings().GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
  const online::Snapshot state = Online().snapshot();
  const bool usable = !ConnectionTestNeeded(ui.settings(), state.testing, TestConfiguration());
  for (const OnlineSection section : OnlinePageSections(on, state.incoming.size(), state.invites.size())) {
    switch (section) {
      case OnlineSection::kOnlinePlay:
        ui.Toggle(kOnlineEnabledLabel, kOnlineEnabledHelp, kOnlineEnabledSetting, kDefaultOnlineEnabled, "Off", "On");
        break;
      case OnlineSection::kFriendRequests: DrawFriendRequests(ui, state); break;
      case OnlineSection::kPartyInvites: DrawPartyInvites(ui, state, usable); break;
      case OnlineSection::kParty: DrawParty(ui, state, usable); break;
      case OnlineSection::kOnlineName: OnlineNameRow(ui); break;
      case OnlineSection::kFriendCode: DrawFriendCode(ui, state); break;
      case OnlineSection::kConnectionTest: DrawConnectionTest(ui, state); break;
      case OnlineSection::kPingDisplay:
        ui.Toggle(kOnlinePingLabel, kOnlinePingHelp, kOnlinePingSetting, kDefaultOnlinePing, "Off", "On");
        break;
      case OnlineSection::kFriends: DrawFriends(ui, state); break;
    }
  }
  ui.EndRows();
}

void OnlinePage::DrawFriendRequests(trg::Ui& ui, const online::Snapshot& state) {
  const trg::Theme& t = ui.theme();
  const float s = ImGui::GetFontSize() / 18.0f;
  ui.Spacer(4);
  for (const online::Request& sender : state.incoming) {
    ImGui::PushID(("request" + sender.player).c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f));
    ImGui::BeginChild("##card", ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(Tr("New friend request"));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
    ImGui::TextUnformatted((sender.name + Tr(" wants to be your friend")).c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2.0f * s));
    if (AcceptButton(ui, Tr("     Accept"))) Online().AcceptFriend(sender.player);
    ImGui::SameLine();
    if (ImGui::Button(Tr("Decline"))) Online().DeclineFriend(sender.player);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ui.Spacer(4);
  }
}

void OnlinePage::DrawPartyInvites(trg::Ui& ui, const online::Snapshot& state, bool usable) {
  const trg::Theme& t = ui.theme();
  const float s = ImGui::GetFontSize() / 18.0f;
  ui.Spacer(4);
  for (const online::Invite& invite : state.invites) {
    ImGui::PushID(("invite" + invite.id).c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f));
    ImGui::BeginChild("##card", ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextUnformatted(Tr("Party invite"));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, t.dim);
    ImGui::TextUnformatted((invite.from_name + Tr(" invited you to a party")).c_str());
    ImGui::PopStyleColor();
    if (!usable) ImGui::TextColored(t.warn_dot, "%s", Tr(kConnectionTestRequired));
    ImGui::Dummy(ImVec2(0, 2.0f * s));
    ImGui::BeginDisabled(!usable || state.party.has_value());
    if (AcceptButton(ui, Tr("     Join"))) Online().AcceptPartyInvite(invite.id);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr("Decline"))) Online().DeclinePartyInvite(invite.id);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ui.Spacer(4);
  }
}

void OnlinePage::DrawParty(trg::Ui& ui, const online::Snapshot& state, bool usable) {
  ui.Heading(state.party ? TrMessage(online::PartyHeadline(state)).c_str() : "Party");
  if (!state.party) {
    ui.Help(kPartyHelp);
    if (!usable) ui.Help(kConnectionTestRequired);
    ui.Row("Online party", kPartyHelp);
    const bool connected = state.connection == online::Connection::kOnline;
    ImGui::BeginDisabled(!usable || !connected);
    if (ImGui::Button(Tr("Start party"))) Online().CreateParty();
    ImGui::EndDisabled();
    return;
  }
  const trg::Theme& t = ui.theme();
  const float s = ImGui::GetFontSize() / 18.0f;
  ui.EndRows();
  for (const online::PartySlotView& slot : online::DescribePartySlots(state)) {
    ImGui::PushID(slot.slot);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 8.0f * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(t.panel.x, t.panel.y, t.panel.z, slot.occupied ? 0.96f : 0.5f));
    const ImVec4 border = slot.problem ? t.warn_dot : slot.you ? t.accent : t.dim;
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(border.x, border.y, border.z, slot.occupied ? 0.7f : 0.3f));
    ImGui::BeginChild("##slot", ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::Text(Tr("Player %d"), slot.slot);
    ImGui::PopFont();
    if (slot.occupied || slot.invited) {
      ImGui::SameLine();
      std::string label = slot.name;
      if (slot.host) label += Tr("  \xC2\xB7  Host");
      if (slot.you) label += Tr("  \xC2\xB7  You");
      ImGui::TextUnformatted(label.c_str());
    }
    std::string detail = Tr(slot.status);
    if (!slot.transport.empty()) detail += " \xC2\xB7 " + Tr(slot.transport);
    if (slot.ping_ms) detail += " \xC2\xB7 " + std::to_string(*slot.ping_ms) + " ms";
    ImGui::PushStyleColor(ImGuiCol_Text, slot.problem ? t.warn_dot : t.dim);
    ImGui::TextUnformatted(detail.c_str());
    ImGui::PopStyleColor();
    if (slot.occupied && !slot.you && state.my_slot == 1) {
      const online::PartyMember* member = state.party->member(slot.slot);
      if (member && ImGui::SmallButton(Tr("Remove from party"))) Online().KickFromParty(member->player);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ui.Spacer(2);
  }
  if (ImGui::Button(Tr("Leave party"))) Online().LeaveParty();
  if (state.my_slot == 1) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Invite friends from the list below, then start the game."));
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("The host starts the match in the game."));
  }
  ui.Spacer(4);
}

void OnlinePage::DrawFriendCode(trg::Ui& ui, const online::Snapshot& state) {
  ui.Row("Your friend code", kFriendCodeHelp);
  if (state.friend_code.empty()) {
    ImGui::TextDisabled("%s", state.connection == online::Connection::kOffline ? Tr("Not available right now") : Tr("Connecting..."));
    return;
  }
  ImGui::TextUnformatted(state.friend_code.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton(Tr("Copy"))) {
    const auto write = [this](const std::string& text, const FieldLog& log) {
      return WriteClipboardText(owner_window_, text, log);
    };
    const bool copied = CopyFriendCode(state.friend_code, write, [](const std::string& line) { Online().LogLine(line); });
    ui.SetStatus(copied ? "Friend code copied." : "Could not copy the friend code.");
  }
}

void OnlinePage::DrawConnectionTest(trg::Ui& ui, const online::Snapshot& state) {
  const bool online_on = ui.settings().GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
  const ConnectionTestView view = DescribeConnectionTestView(
      online_on, ConnectionTestNeeded(ui.settings(), state.testing, TestConfiguration()), state.testing, kOnWindows);
  const ConnectionTestAttention::Step step = attention_.Frame(ImGui::GetTime(), view.highlighted);
  if (!view.highlighted) {
    DrawConnectionTestBody(ui, state, view, false);
    return;
  }
  // The friend request card's look, in the accent colour; the border brightens while the attention step runs.
  const trg::Theme& t = ui.theme();
  const float s = ImGui::GetFontSize() / 18.0f;
  const float glow = step.strength;
  ui.EndRows();
  ui.Spacer(4);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, (2.0f + 2.0f * glow) * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 10.0f * s));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(t.panel.x + (t.accent.x - t.panel.x) * (0.05f + 0.10f * glow),
                                                 t.panel.y + (t.accent.y - t.panel.y) * (0.05f + 0.10f * glow),
                                                 t.panel.z + (t.accent.z - t.panel.z) * (0.05f + 0.10f * glow), 0.96f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.8f + 0.2f * glow));
  ImGui::BeginChild("##connection_test_required", ImVec2(-FLT_MIN, 0),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY |
                        ImGuiChildFlags_NavFlattened);
  DrawConnectionTestBody(ui, state, view, step.focus_run_test);
  ui.EndRows();
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  ui.Spacer(4);
  if (step.scroll_into_view) {
    const float visible_top = ImGui::GetWindowPos().y;
    const float visible_bottom = visible_top + ImGui::GetWindowHeight();
    const float margin = 8.0f * s;
    const float above = visible_top + margin - ImGui::GetItemRectMin().y;
    const float below = ImGui::GetItemRectMax().y + margin - visible_bottom;
    if (above > 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() - above);
    else if (below > 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() + below);
  }
}

void OnlinePage::DrawConnectionTestBody(trg::Ui& ui, const online::Snapshot& state, const ConnectionTestView& view,
                                        bool focus_run_test) {
  if (focus_run_test) ImGui::SetKeyboardFocusHere();
  if (ui.ButtonRow("Connection test", kTestHelp, state.testing ? "Testing..." : "Run test", view.primary_button) &&
      !state.testing) {
    Online().TestConnection();
  }
  if (view.required_text) {
    ui.EndRows();
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::TextColored(ui.theme().accent, "%s", Tr(kConnectionTestRequiredLabel));
    ImGui::PopFont();
  }
  if (view.windows_note) {
    if (const char* note = ConnectionTestRunningNote(true)) ui.Help(note);
  }
  if (!state.test || state.testing) return;
  ui.EndRows();
  ui.Heading(TrMessage(state.test->verdict).c_str());
  for (const online::ConnectionReport::Line& line : state.test->lines) {
    using Mark = online::ConnectionReport::Mark;
    const ImVec4 color = line.mark == Mark::kOk ? ui.theme().good_dot
                         : line.mark == Mark::kNote ? ui.theme().dim : ui.theme().warn_dot;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(line.mark == Mark::kOk ? "OK" : line.mark == Mark::kNote ? "--" : "!!");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s: %s", Tr(line.label.c_str()), TrMessage(line.text).c_str());
  }
  ui.Spacer();
}

void OnlinePage::DrawFriends(trg::Ui& ui, const online::Snapshot& state) {
  ui.Heading("Friends");
  if (state.connection == online::Connection::kOffline || state.connection == online::Connection::kError) {
    ui.Help(state.connection_text.empty() ? kUnreachable : TrMessage(state.connection_text).c_str());
  } else if (state.connection == online::Connection::kChecking) {
    ui.Help(state.connection_text.empty() ? "Connecting..." : TrMessage(state.connection_text).c_str());
  } else if (!state.notice.empty()) {
    ui.Help(TrMessage(state.notice).c_str());
  }
  ui.Row("Add a friend", "Their friend code, for example ABCD-EFGH.");
  add_field_.Draw(add_code_, sizeof(add_code_), [](const std::string& line) { Online().LogLine(line); });
  const std::string code = online::NormalizeFriendCode(add_code_);
  ImGui::SameLine();
  ImGui::BeginDisabled(code.empty());
  const bool clicked = ImGui::Button(Tr("Send request"));
  ImGui::EndDisabled();
  if (clicked && !code.empty() && Online().AddFriend(code) == online::FriendRequestResult::kSent) add_code_[0] = 0;
  if (code.empty() && std::strlen(add_code_) >= 8) ImGui::TextDisabled("%s", Tr("That is not a valid friend code."));
  for (const online::Request& r : state.outgoing) {
    ui.Row(r.name.c_str(), "Your request is waiting for an answer.");
    ImGui::PushID(("out" + r.player).c_str());
    if (ImGui::Button(Tr("Cancel request"))) Online().DeclineFriend(r.player);
    ImGui::PopID();
  }
  for (const online::Friend& f : state.friends) {
    const online::FriendRowView view = online::DescribeFriend(state, f);
    FriendHeading(ui, f.name, view);
    ImGui::PushID(("f" + f.player).c_str());
    const bool can_invite = state.party && state.my_slot == 1 && f.online && f.compatible &&
                            f.state == online::FriendState::kOnline &&
                            state.party->members.size() + state.party->invited.size() < size_t(online::kMaxParty);
    if (can_invite) {
      if (ImGui::Button(Tr("Invite"))) Online().InviteToParty(f.player);
      ImGui::SameLine();
    }
    if (ImGui::Button(Tr("Remove"))) Online().RemoveFriend(f.player);
    ImGui::PopID();
  }
  if (state.fetched && state.friends.empty() && state.incoming.empty() && state.outgoing.empty()) {
    ui.EndRows();
    ui.Help(kNoFriends);
  }
  ui.EndRows();
}

}  // namespace tmnt
