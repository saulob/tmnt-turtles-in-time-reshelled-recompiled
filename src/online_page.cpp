#include "online_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

#include <imgui.h>

#include "friend_code_field.h"
#include "localization/ui_text.h"
#include "online/friend_view.h"
#include "online/identity.h"
#include "online/local_addresses.h"
#include "online/protocol.h"
#include "online_layout.h"
#include "online_runtime.h"
#include "online_setting.h"

namespace tmnt {
namespace {

constexpr char kFriendCodeHelp[] = "Share this code so another player can add you.";
constexpr char kTestHelp[] =
    "Checks that you can reach the online service and that other players can reach you. Takes a few seconds.";
constexpr char kUnreachable[] = "Cannot reach the online service right now. Run the Connection test for details.";
constexpr char kNoFriends[] = "No friends yet. A friend in a room appears in the game's Online menu.";
constexpr char kLobbyEmpty[] = "No players are currently visible in the Lobby.";

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

bool AcceptButton(trg::Ui& ui, const char* id, ImVec2 size = ImVec2(0, 0)) {
  const trg::Theme& t = ui.theme();
  trg::PushActionButton(t, ui.scale());
  const float mark = ImGui::GetFontSize() * 0.8f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Button(id, size);
  const ImVec2 pad = ImGui::GetStyle().FramePadding;
  const ImVec2 origin(at.x + pad.x, at.y + pad.y + ImGui::GetFontSize() * 0.1f);
  const ImVec2 tick[3] = {ImVec2(origin.x + mark * 0.1f, origin.y + mark * 0.55f),
                          ImVec2(origin.x + mark * 0.4f, origin.y + mark * 0.85f),
                          ImVec2(origin.x + mark * 0.95f, origin.y + mark * 0.2f)};
  ImGui::GetWindowDrawList()->AddPolyline(tick, 3, ImGui::GetColorU32(t.on_action), ImDrawFlags_None, mark * 0.18f);
  trg::PopActionButton();
  return clicked;
}

// The name, then the status line in the row's help style under it.
void FriendHeading(trg::Ui& ui, const std::string& name, const online::FriendRowView& view) {
  ui.RowLeft(name.c_str());
  ImGui::PushFont(nullptr, trg::layout::kTextHelp);
  ImGui::TextColored(ui.theme().dim, "%s", TrMessage(view.status).c_str());
  if (view.incompatible) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ui.theme().dim, " \xC2\xB7 ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ui.theme().warn_dot, "%s", Tr("Incompatible"));
  }
  ImGui::PopFont();
  ui.RowControl();
}

void BeginCard(trg::Ui& ui, const char* id, const ImVec4& background, const ImVec4& border, float border_width,
               ImGuiChildFlags flags = 0, float height = 0.0f) {
  const float s = ui.scale();
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border_width * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kCardPaddingX * s, kCardPaddingY * s));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  const ImGuiChildFlags size_flags = height > 0.0f ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY;
  ImGui::BeginChild(id, ImVec2(-FLT_MIN, height),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | size_flags | flags);
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

// Why the service itself cannot be used right now; empty once it is online.
std::string ServiceNotice(const online::Snapshot& state) {
  if (state.connection == online::Connection::kOffline || state.connection == online::Connection::kError) {
    return state.connection_text.empty() ? Tr(kUnreachable) : TrMessage(state.connection_text);
  }
  if (state.connection == online::Connection::kChecking) {
    return state.connection_text.empty() ? Tr("Connecting...") : TrMessage(state.connection_text);
  }
  return {};
}

std::string LobbyUpdatedLabel(int minutes) {
  if (minutes < 1) return Tr("Updated just now");
  std::string text = Tr("Updated %s min ago");
  if (const size_t at = text.find("%s"); at != std::string::npos) {
    text.replace(at, 2, std::to_string(minutes));
  }
  return text;
}

}  // namespace

std::vector<std::string> OnlinePage::Tick(trg::SettingsStore& settings) {
  online::UserSettings edited;
  edited.enabled = settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
  edited.name = settings.Get(kOnlineNameSetting, kDefaultOnlineNameValue);
  edited.lobby_visible = settings.GetBool(kOnlineLobbySetting, kDefaultOnlineLobby);
  tabs_.TrackOnlinePlay(edited.enabled);
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
  for (const OnlineSection section : OnlinePageSections(on, tabs_.selected(), state.incoming.size(), state.invites.size())) {
    switch (section) {
      case OnlineSection::kOnlinePlay:
        ui.Toggle(kOnlineEnabledLabel, kOnlineEnabledHelp, kOnlineEnabledSetting, kDefaultOnlineEnabled, "Off", "On");
        break;
      case OnlineSection::kTabs: DrawTabs(ui, state.incoming.size()); break;
      case OnlineSection::kFriendRequests: DrawFriendRequests(ui, state); break;
      case OnlineSection::kPartyInvites: DrawPartyInvites(ui, state, usable); break;
      case OnlineSection::kOnlineName: OnlineNameRow(ui); break;
      case OnlineSection::kFriendCode: DrawFriendCode(ui, state); break;
      case OnlineSection::kConnectionTest: DrawConnectionTest(ui, state); break;
      case OnlineSection::kPingDisplay:
        ui.Toggle(kOnlinePingLabel, kOnlinePingHelp, kOnlinePingSetting, kDefaultOnlinePing, "Off", "On");
        break;
      case OnlineSection::kFriends: DrawFriends(ui, state); break;
      case OnlineSection::kLobby: DrawLobby(ui, state); break;
    }
  }
  ui.EndRows();
}

// Plain buttons, so keyboard navigation and its highlight work as anywhere else on the page.
void OnlinePage::DrawTabs(trg::Ui& ui, size_t friend_requests) {
  ui.EndRows();
  ui.Spacer(4);
  const trg::Theme& t = ui.theme();
  const float s = ui.scale();
  const float left = ImGui::GetCursorScreenPos().x;
  const float width = ImGui::GetContentRegionAvail().x;
  float bottom = 0.0f;
  float selected_left = 0.0f;
  float selected_right = 0.0f;
  ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 7.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.frame_hot);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.frame_active);
  for (const OnlineTab tab : kOnlineTabs) {
    const bool selected = tab == tabs_.selected();
    if (tab != kOnlineTabs[0]) ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushID(int(tab));
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? t.accent : t.dim);
    if (ImGui::Button(OnlineTabTitle(Tr(OnlineTabLabel(tab)), tab, friend_requests).c_str())) tabs_.Select(tab);
    ImGui::PopStyleColor();
    ImGui::PopID();
    bottom = ImGui::GetItemRectMax().y;
    if (selected) {
      selected_left = ImGui::GetItemRectMin().x;
      selected_right = ImGui::GetItemRectMax().x;
    }
  }
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  ImGui::PopFont();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float base = std::floor(bottom);
  draw->AddRectFilled(ImVec2(left, base - 1.0f), ImVec2(left + width, base), trg::Col(t.border_row));
  const float thick = std::max(2.0f, std::round(2.0f * s));
  draw->AddRectFilled(ImVec2(selected_left, base - thick), ImVec2(selected_right, base), trg::Col(t.accent));
  ui.Spacer(4);
}

void OnlinePage::DrawFriendRequests(trg::Ui& ui, const online::Snapshot& state) {
  const trg::Theme& t = ui.theme();
  const float s = ImGui::GetFontSize() / 18.0f;
  ui.Spacer(4);
  for (const online::Request& sender : state.incoming) {
    ImGui::PushID(("request" + online::RequestKey(sender)).c_str());
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
    if (AcceptButton(ui, Tr("     Accept"))) Online().AcceptFriend(online::RequestKey(sender));
    ImGui::SameLine();
    if (ImGui::Button(Tr("Decline"))) Online().DeclineFriend(online::RequestKey(sender));
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
    ImGui::TextUnformatted(Tr("Start the game to accept this invite."));
    if (ImGui::Button(Tr("Decline"))) Online().DeclinePartyInvite(invite.id);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ui.Spacer(4);
  }
}

void OnlinePage::DrawFriendCode(trg::Ui& ui, const online::Snapshot& state) {
  ui.RowEmphasized("Your friend code", kFriendCodeHelp);
  ImGui::AlignTextToFramePadding();
  if (state.friend_code.empty()) {
    ImGui::TextDisabled("%s", state.connection == online::Connection::kOffline ? Tr("Not available right now")
                                                                               : Tr("Connecting..."));
    return;
  }
  ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
  ImGui::TextUnformatted(state.friend_code.c_str());
  ImGui::PopFont();
  ImGui::SameLine();
  if (ImGui::Button(Tr("Copy"), ImVec2(-FLT_MIN, 0))) {
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
  const char* button = state.testing ? "Testing..." : "Run test";
  bool run = false;
  if (view.highlighted) {
    ui.RowLeft("Connection test", kTestHelp);
    if (view.required_text) {
      ImGui::PushFont(ui.fonts().semibold, 0.0f);
      ImGui::TextColored(ui.theme().accent, "%s", Tr(kConnectionTestRequiredLabel));
      ImGui::PopFont();
    }
    ui.RowControl();
    ImGui::PushID("Connection test");
    if (focus_run_test) ImGui::SetKeyboardFocusHere();
    run = view.primary_button ? ui.AccentButton(button) : ImGui::Button(button, ImVec2(-FLT_MIN, 0));
    ImGui::PopID();
  } else {
    if (focus_run_test) ImGui::SetKeyboardFocusHere();
    run = ui.ButtonRow("Connection test", kTestHelp, button);
  }
  if (run && !state.testing) Online().TestConnection();
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
  const std::string notice = ServiceNotice(state);
  if (!notice.empty()) ui.Help(notice.c_str());
  else if (!state.notice.empty()) ui.Help(TrMessage(state.notice).c_str());
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
    ImGui::PushID(("out" + online::RequestKey(r)).c_str());
    if (ImGui::Button(Tr("Cancel request"))) Online().DeclineFriend(online::RequestKey(r));
    ImGui::PopID();
  }
  for (const online::Friend& f : state.friends) {
    const online::FriendRowView view = online::DescribeFriend(state, f);
    FriendHeading(ui, f.name, view);
    ImGui::PushID(("f" + f.player).c_str());
    if (ImGui::Button(Tr("Remove"))) Online().RemoveFriend(f.player);
    ImGui::PopID();
  }
  if (state.fetched && state.friends.empty() && state.incoming.empty() && state.outgoing.empty()) {
    ui.EndRows();
    ui.Help(kNoFriends);
  }
  ui.EndRows();
}

void OnlinePage::DrawLobby(trg::Ui& ui, const online::Snapshot& state) {
  DrawLobbyVisibility(ui);
  if (!ui.settings().GetBool(kOnlineLobbySetting, kDefaultOnlineLobby)) return;
  DrawLobbyPlayers(ui, state);
}

void OnlinePage::DrawLobbyVisibility(trg::Ui& ui) {
  const bool visible = ui.settings().GetBool(kOnlineLobbySetting, kDefaultOnlineLobby);
  ui.Toggle(kOnlineLobbyLabel, visible ? kOnlineLobbyOnHelp : kOnlineLobbyOffHelp, kOnlineLobbySetting,
            kDefaultOnlineLobby, "Off", "On");
  ui.EndRows();
}

void OnlinePage::DrawLobbyPlayers(trg::Ui& ui, const online::Snapshot& state) {
  Online().NoteLobbyViewed();
  const std::vector<const online::LobbyPlayer*> players = online::VisibleLobbyPlayers(state.lobby);
  DrawLobbyHeader(ui, state, players.size());
  std::string notice = ServiceNotice(state);
  if (notice.empty()) notice = online::LobbyMessage(state.lobby);
  if (!notice.empty()) ui.Help(TrMessage(notice).c_str());
  else if (!state.notice.empty()) ui.Help(TrMessage(state.notice).c_str());
  if (!state.lobby.supported) return;
  DrawLobbyList(ui, state, players);
}

void OnlinePage::DrawLobbyHeader(trg::Ui& ui, const online::Snapshot& state, size_t player_count) {
  const std::string title = std::string(Tr("Players in Lobby")) + " (" + std::to_string(player_count) + ")";
  ui.Heading(title.c_str());
  const float title_height = ImGui::GetItemRectSize().y;
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  const ImGuiStyle& style = ImGui::GetStyle();
  const char* idle = Tr("Refresh lobby");
  const char* busy = Tr("Refreshing...");
  const float button_width =
      std::max(ImGui::CalcTextSize(idle).x, ImGui::CalcTextSize(busy).x) + style.FramePadding.x * 2.0f;
  float left = right - button_width;
  ImGui::SameLine();
  const float line = ImGui::GetCursorPosY();
  if (state.lobby.fetched && state.lobby.updated_seconds_ago >= 0.0) {
    const int minutes = int(state.lobby.updated_seconds_ago / 60.0);
    const std::string text = LobbyUpdatedLabel(minutes);
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    left -= size.x + style.ItemSpacing.x;
    ImGui::SameLine(left);
    ImGui::SetCursorPosY(line + (title_height - size.y) * 0.5f);
    ImGui::TextColored(ui.theme().dim, "%s", text.c_str());
    ImGui::PopFont();
  }
  const bool refreshing = state.lobby.refreshing;
  ImGui::SameLine(right - button_width);
  ImGui::SetCursorPosY(line + (title_height - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::BeginDisabled(refreshing || !state.lobby.supported || state.connection != online::Connection::kOnline);
  if (ImGui::Button((std::string(refreshing ? busy : idle) + "###refresh_lobby").c_str(), ImVec2(button_width, 0))) {
    Online().RefreshLobby();
  }
  ImGui::EndDisabled();
}

void OnlinePage::DrawLobbyList(trg::Ui& ui, const online::Snapshot& state,
                               const std::vector<const online::LobbyPlayer*>& players) {
  const trg::Theme& t = ui.theme();
  const float s = ui.scale();
  ui.Spacer(4);
  const float height = LobbyCardHeight(ImGui::GetContentRegionAvail().y, players.size(), s);
  ImGui::PushID("lobby-list");
  BeginCard(ui, "##lobby_list", ImVec4(t.panel.x, t.panel.y, t.panel.z, 0.96f), t.border_panel, 1.0f,
            ImGuiChildFlags_NavFlattened, height);
  if (!state.lobby.fetched) {
    if (state.connection == online::Connection::kOnline && !state.lobby.failed) ui.Help(Tr("Connecting..."));
  } else if (players.empty()) {
    ui.Help(Tr(kLobbyEmpty));
  } else {
    for (const online::LobbyPlayer* player : players) DrawLobbyPlayer(ui, state, *player);
    ui.EndRows();
  }
  EndCard();
  ImGui::PopID();
}

void OnlinePage::DrawLobbyPlayer(trg::Ui& ui, const online::Snapshot& state, const online::LobbyPlayer& player) {
  const online::LobbyRowView view = online::DescribeLobbyPlayer(state, player);
  const online::LobbyAction action = online::LobbyActionOf(view.relation);
  ui.RowLeft(player.name.c_str());
  ImGui::PushFont(nullptr, trg::layout::kTextHelp);
  ImGui::TextColored(ui.theme().dim, "%s", Tr(online::LobbyVersionLabel(player)).c_str());
  ImGui::PopFont();
  ui.RowControl();
  ImGui::PushID(("lobby" + player.player).c_str());
  const bool enabled = view.relation == online::LobbyRelation::kNone ? view.can_add : action.enabled;
  const auto add = [&] {
    if (online::IsLobbyHandle(player.player)) Online().AddFriendFromLobby(player.player);
    else Online().AddFriend(player.player);
  };
  const auto decline = [&] {
    if (online::IsLobbyHandle(player.player)) Online().DeclineLobbyFriend(player.player);
    else Online().DeclineFriend(player.player);
  };
  if (!enabled) {
    ImGui::BeginDisabled();
    ImGui::Button(Tr(action.text), ImVec2(-FLT_MIN, 0));
    ImGui::EndDisabled();
  } else if (action.decline) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    if (AcceptButton(ui, Tr("     Accept"), ImVec2(half, 0))) add();
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button(Tr("Decline"), ImVec2(half, 0))) decline();
  } else if (ImGui::Button(Tr(action.text), ImVec2(-FLT_MIN, 0))) {
    add();
  }
  ImGui::PopID();
}

}  // namespace tmnt
