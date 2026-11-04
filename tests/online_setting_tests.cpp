// Online settings: Online play defaults to Off, the online name persists, both reach the runtime cvars and the
// launcher, no gameplay setting is locked while Online play is on, the Ping display toggle, and PLAY waiting for a
// finished Connection test. Headless: no window, no game runtime.
// Run: tmnt_online_setting_tests

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "connection_test_gate.h"
#include "corner_overlay.h"
#include "legacy_settings.h"
#include "online_page.h"
#include "online_setting.h"
#include "ping_display.h"

#include "trg/settings.h"

using namespace tmnt;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  std::string source = text.str();
  source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
  return source;
}

std::string ReadSource(const char* name) { return ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / "src" / name); }

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void TestOnlineDefaultsToOff() {
  CHECK(!kDefaultOnlineEnabled);
  CHECK(!kDefaultOnlinePing);
  CHECK(std::string_view(kOnlineEnabledSetting) == "tmnt_online_enabled");
  CHECK(std::string_view(kOnlineNameSetting) == "tmnt_online_name");
  CHECK(std::string_view(kOnlinePingSetting) == "tmnt_online_ping_display");
  CHECK(std::string_view(kDefaultOnlineNameValue) == "Player");
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(Has(runtime, "REXCVAR_DEFINE_BOOL(tmnt_online_enabled, tmnt::kDefaultOnlineEnabled, \"TMNT\""));
  CHECK(Has(runtime, "REXCVAR_DEFINE_STRING(tmnt_online_name, tmnt::kDefaultOnlineNameValue, \"TMNT\""));
  CHECK(Has(runtime, "REXCVAR_DEFINE_BOOL(tmnt_online_ping_display, tmnt::kDefaultOnlinePing, \"TMNT\""));
}

void TestOnlineNamePersists() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tmnt_online_setting_tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "settings.toml").string();
  std::ofstream(path) << "# player settings\n";

  CHECK(!trg::TextFileSettings(path).GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled));
  CHECK(trg::TextFileSettings(path).Get(kOnlineNameSetting, kDefaultOnlineNameValue) == "Player");

  trg::TextFileSettings writer(path);
  writer.Set(kOnlineEnabledSetting, "true");
  writer.Set(kOnlineNameSetting, "Saulo");
  writer.Set(kOnlinePingSetting, "true");
  CHECK(writer.Save());
  CHECK(trg::TextFileSettings(path).GetBool(kOnlineEnabledSetting, false));
  CHECK(trg::TextFileSettings(path).GetBool(kOnlinePingSetting, false));
  CHECK(trg::TextFileSettings(path).Get(kOnlineNameSetting, "") == "Saulo");
  const std::string saved = ReadFile(path);
  CHECK(Has(saved, "tmnt_online_name = \"Saulo\"") || Has(saved, "tmnt_online_name = Saulo"));
  std::filesystem::remove_all(dir);
}

void TestSettingsReachTheRuntime() {
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "      kOnlineEnabledSetting,\n"));
  CHECK(Has(settings, "      kOnlineNameSetting,\n"));
  CHECK(Has(settings, "      kOnlinePingSetting,\n"));
}

// This port has no cheats and no setting that changes what the game simulates, so Online play locks none of them.
void TestNoSettingIsLockedWhileOnline() {
  CHECK(!Has(ReadSource("launcher.cpp"), "kOnlineEnabledSetting"));
  CHECK(!Has(ReadSource("settings.cpp"), "tmnt_online_enabled"));
}

void TestPingDisplayRules() {
  CHECK(PingText(32) == "Ping: 32 ms");
  CHECK(PingText(std::nullopt) == "Ping: --");
  CHECK(!PingVisible(false, true));
  CHECK(!PingVisible(true, false));
  CHECK(PingVisible(true, true));
  // Without a connected peer the readout stays up and says "--".
  CHECK(PingText(MeasuredRtt(online::NetplayLink::kNone, 40)) == "Ping: --");
  CHECK(PingText(MeasuredRtt(online::NetplayLink::kConnecting, 40)) == "Ping: --");
  CHECK(PingText(MeasuredRtt(online::NetplayLink::kConnected, std::nullopt)) == "Ping: --");
  CHECK(PingText(MeasuredRtt(online::NetplayLink::kConnected, 42)) == "Ping: 42 ms");
  CHECK(PingText(MeasuredRtt(online::NetplayLink::kFighting, 42)) == "Ping: 42 ms");
  CHECK(std::string_view(kPingDisplayKey) == "F10");
  const std::string ping = ReadSource("ping_display.cpp");
  CHECK(Has(ping, "kPingDisplayKey") && !Has(ping, "\"F9\""));
  CHECK(Has(ping, "Online().WorstRtt()"));
  CHECK(Has(ping, "DrawCornerText(PingText(Online().WorstRtt()), PingSlot(counter));"));
  CHECK(Has(ReadSource("frame_counter.cpp"), "DrawCornerText(FrameCounterText(stats), FrameCounterSlot());"));
  CHECK(!Has(ping, "AddRect"));
}

void TestCornerReadoutsStackWithoutOverlap() {
  CHECK(FrameCounterSlot() == 0 && PingSlot(true) == 1 && PingSlot(false) == 0);
  for (const float scale : {1.0f, 1.5f, 2.25f}) {
    const float font = 18.0f * scale;
    const float width = 1920.0f * scale;
    const CornerPanel fps = CornerPanelAt(width, font, 9.0f * font, font, FrameCounterSlot());
    const CornerPanel ping = CornerPanelAt(width, font, 6.0f * font, font, PingSlot(true));
    const CornerPanel alone = CornerPanelAt(width, font, 6.0f * font, font, PingSlot(false));
    CHECK(fps.x1 == ping.x1 && fps.x1 == width - font);
    CHECK(ping.y0 > fps.y1 && ping.y0 - fps.y1 == font * 0.25f);
    CHECK(ping.y1 - ping.y0 == fps.y1 - fps.y0);
    CHECK(alone.y0 == fps.y0 && alone.x1 == fps.x1);
    CHECK(fps.x0 > 0 && fps.y0 == font);
    CHECK(std::abs(ping.text_x - ping.x0 - font * 0.6f) < 0.01f && std::abs(ping.text_y - ping.y0 - font * 0.3f) < 0.01f);
  }
}

void TestNoDebugOverlayText() {
  CHECK(Has(ReadSource("launcher.cpp"), "\"Shows the game's frame rate in the corner. F2 toggles it while playing.\""));
  for (const char* source : {"launcher.cpp", "online_page.cpp", "online_setting.h", "ping_display.h"}) {
    const std::string text = ReadSource(source);
    CHECK(!Has(text, "F9") && !Has(text, "F4 debug") && !Has(text, "F4 settings") && !Has(text, "debug overlay"));
    CHECK(!Has(text, "settings overlay"));
  }
  for (const char* doc : {"README.md", "docs/online.md"}) {
    const std::string text = ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / doc);
    CHECK(!Has(text, "**F9**") && !Has(text, "F9 ") && !Has(text, "debug overlay") && !Has(text, "F4 settings") &&
          !Has(text, "F3 debug"));
  }
  CHECK(Has(ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / "docs/online.md"), "**F10** shows or hides it"));
  CHECK(Has(ReadSource("keybinds.h"), "case ImGuiKey_F10:"));
}

void TestOnlinePageSections() {
  CHECK(OnlinePageSections(false, 3) == std::vector<OnlineSection>{OnlineSection::kOnlinePlay});
  const std::vector<OnlineSection> on = OnlinePageSections(true, 0);
  CHECK((on == std::vector<OnlineSection>{OnlineSection::kOnlinePlay, OnlineSection::kOnlineName,
                                          OnlineSection::kFriendCode, OnlineSection::kConnectionTest,
                                          OnlineSection::kPingDisplay, OnlineSection::kParty,
                                          OnlineSection::kFriends}));
  const std::vector<OnlineSection> with_requests = OnlinePageSections(true, 2);
  CHECK(with_requests.size() == 8 && with_requests[1] == OnlineSection::kFriendRequests);
  const std::vector<OnlineSection> with_invites = OnlinePageSections(true, 0, 1);
  CHECK(with_invites.size() == 8 && with_invites[1] == OnlineSection::kPartyInvites);
  const std::vector<OnlineSection> with_both = OnlinePageSections(true, 1, 1);
  CHECK(with_both.size() == 9 && with_both[1] == OnlineSection::kFriendRequests &&
        with_both[2] == OnlineSection::kPartyInvites && with_both[7] == OnlineSection::kParty);
}

void TestLauncherHasAnOnlinePageWithoutTechnicalControls() {
  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "{\"Online\", "));
  CHECK(Has(launcher, "online_page_.Draw(ui)"));
  CHECK(Has(launcher, "online_page_.Tick(settings_)"));
  CHECK(Has(launcher, "online_page_.NeedsAttention()"));
  const std::string page = ReadSource("online_page.cpp");
  for (const char* shown : {"Add a friend", "Send request", "Accept", "Decline", "Run test", "Your friend code", "Copy",
                            "Remove"}) {
    CHECK(Has(page, shown));
  }
  for (const char* hidden : {"7001", "7002", "wss://", "workers.dev", "token", "Lobby", "lobby", "IP address",
                             "Challenge", "challenge", "Disconnect", "wants to play", "Waiting for"}) {
    CHECK(!Has(page, hidden));
  }
  const std::string view = ReadSource("online/friend_view.cpp");
  CHECK(!Has(view, "hallenge") && !Has(view, "Press Play"));
}

void TestFriendCodeCopyAndPasteUseTheNativeClipboard() {
  const std::string page = ReadSource("online_page.cpp");
  const std::string header = ReadSource("online_page.h");
  const std::string field = ReadSource("friend_code_field.cpp");
  const std::string win32 = ReadSource("clipboard_win32.cpp");
  CHECK(Has(header, "char add_code_[kFriendCodeFieldSize] = {};"));
  CHECK(Has(header, "return ReadClipboardText(owner_window_, log);"));
  CHECK(Has(page, "add_field_.Draw(add_code_, sizeof(add_code_)"));
  CHECK(Has(page, "return WriteClipboardText(owner_window_, text, log);"));
  CHECK(Has(page, "CopyFriendCode(state.friend_code, write"));
  CHECK(Has(ReadSource("launcher.cpp"), "online_page_.SetOwnerWindow(WindowHandle());"));
  CHECK(!Has(page, "ImGui::SetClipboardText") && !Has(page, "SDL_"));
  CHECK(!Has(field, "Clipboard(") && !Has(field, "SDL_") && !Has(field, "windows.h"));
  CHECK(Has(field, "ImGui::InputTextWithHint(\"##add_code\", \"XXXX-XXXX\""));
  CHECK(Has(field, "ImGui::IsKeyDown(ImGuiKey_LeftCtrl)") && Has(field, "ImGui::IsKeyPressed(ImGuiKey_V, false)"));
  for (const char* call : {"::OpenClipboard(owner)", "::CloseClipboard()", "::EmptyClipboard()",
                           "::IsClipboardFormatAvailable(format)", "::GetClipboardData(format)",
                           "::SetClipboardData(format, memory)", "::GlobalAlloc(flags, bytes)", "::GlobalLock(memory)",
                           "::GlobalUnlock(memory)", "::GetLastError()", "CF_UNICODETEXT", "MB_ERR_INVALID_CHARS",
                           "WC_ERR_INVALID_CHARS"}) {
    CHECK(Has(win32, call));
  }
  CHECK(!Has(win32, "SDL_"));
  const std::string cmake = ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / "CMakeLists.txt");
  CHECK(Has(cmake, "list(APPEND TMNT_SOURCES src/clipboard_win32.cpp)"));
  for (const char* gone : {"imgui_clipboard.cpp", "sdl_clipboard.cpp"}) {
    CHECK(!std::filesystem::exists(std::filesystem::path(TMNT_SOURCE_DIR) / "src" / gone));
    CHECK(!Has(cmake, gone));
  }
  CHECK(Has(page, "online::NormalizeFriendCode(add_code_)"));
  CHECK(Has(page, "ImGui::BeginDisabled(code.empty());"));
  CHECK(Has(page, "Online().AddFriend(code) == online::FriendRequestResult::kSent"));
  const size_t field_at = page.find("add_field_.Draw(");
  const size_t normalized = page.find("online::NormalizeFriendCode(add_code_)");
  const size_t button = page.find("ImGui::Button(\"Send request\")");
  CHECK(field_at != std::string::npos && field_at < normalized && normalized < button);
}

void TestConnectionTestPageShowsTheReport() {
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "ui.Heading(state.test->verdict.c_str());"));
  CHECK(Has(page, "online::ConnectionReport::Line& line : state.test->lines"));
  CHECK(Has(page, "Checks that you can reach the online service and that other players can reach you."));
  const std::string test = ReadSource("online/connection_test.cpp");
  for (const char* label : {"\"Online service\"", "\"Direct connections\"", "\"Router configuration\"", "Ready for online play",
                            "Limited: direct connections may be blocked by your network"}) {
    CHECK(Has(test, label));
  }
  CHECK(!Has(test, "localhost") && !Has(test, "127.0.0.1"));
  // STUN, UPnP and router details go to the log, not the page.
  CHECK(Has(ReadSource("online/online_service.cpp"),
            "for (const std::string& detail : finished->details) Log(\"DEBUG_SAULO online connection test \" + detail);"));
  for (const char* logged : {"\"STUN \" + host", "upnp + status.detail", "double NAT or "}) CHECK(Has(test, logged));
  CHECK(Has(ReadSource("online/stun.h"), "stun.cloudflare.com:3478,stun.l.google.com:19302"));
  CHECK(Has(ReadSource("online/online_service.h"), "wss://tmnt-lobby.saulobenigno.workers.dev/"));
}

void TestLocalAndOnlineNamesAreSeparate() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tmnt_online_name_tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "settings.toml").string();
  std::ofstream(path) << "# player settings\n";
  trg::TextFileSettings file(path);
  file.Set(kOnlineNameSetting, "Saulo");
  file.Set("tmnt_player_2_name", "teste2");
  file.Set("tmnt_player_1_name", "saulo1");
  CHECK(file.Get(kOnlineNameSetting, kDefaultOnlineNameValue) == "Saulo");
  file.Set(kOnlineNameSetting, "Saulo-2");
  CHECK(file.Get("tmnt_player_1_name", "Player 1") == "saulo1");
  CHECK(file.Get("tmnt_player_2_name", "Player 2") == "teste2");
  CHECK(file.Save());
  trg::TextFileSettings reread(path);
  CHECK(reread.Get(kOnlineNameSetting, "") == "Saulo-2" && reread.Get("tmnt_player_1_name", "") == "saulo1" &&
        reread.Get("tmnt_player_2_name", "") == "teste2");
  std::filesystem::remove_all(dir);

  // The online identity reads only the Online name; the local names only feed the SDK's local players.
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "edited.name = settings.Get(kOnlineNameSetting, kDefaultOnlineNameValue);"));
  for (const char* source : {"online_page.cpp", "online_runtime.cpp", "online/online_service.cpp",
                             "player_match_hook.cpp", "player_match_net_hook.cpp"}) {
    const std::string text = ReadSource(source);
    CHECK(!Has(text, "tmnt_player_1_name") && !Has(text, "tmnt_player_2_name") && !Has(text, "kPlayerNames"));
    CHECK(!Has(text, "tmnt_player_3_name") && !Has(text, "tmnt_player_4_name"));
    CHECK(!Has(text, "local_player_1_name") && !Has(text, "local_player_2_name"));
  }
  CHECK(Has(ReadSource("player_match_hook.cpp"), "live.name = profile.name;"));
  CHECK(Has(ReadSource("player_match_hook.cpp"), "const tmnt::online::OnlineProfile profile = tmnt::Online().Profile();"));
  const std::string settings = ReadSource("settings.cpp");
  const size_t apply = settings.find("void ApplyPlayerNames() {");
  const std::string apply_names = settings.substr(apply, settings.find("\n}\n", apply) - apply);
  CHECK(!Has(apply_names, "online") && !Has(apply_names, "Online"));
}

void TestOnlineNameFieldMatchesItsHelp() {
  CHECK(std::string_view(kOnlineNameLabel) == "Online name");
  CHECK(std::string_view(kOnlineNameHelp) == "What other players see: up to 16 letters, digits, - or _.");
  CHECK(online::kMaxOnlineNameChars == 16);
  for (int c = 0; c < 256; ++c) {
    const bool listed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    CHECK(online::OnlineNameChar(char(c)) == listed);
  }
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "case OnlineSection::kOnlineName: OnlineNameRow(ui); break;"));
  CHECK(Has(page, "char text[online::kMaxOnlineNameChars + 1] = {};"));
  CHECK(Has(page, "ImGuiInputTextFlags_CallbackCharFilter, OnlineNameFilter"));
  CHECK(Has(page, "online::OnlineNameChar(char(data->EventChar))"));
  CHECK(Has(page, "\"Other players see \" + shown"));
  CHECK(Has(ReadSource("online/identity.cpp"), "if (OnlineNameChar(c)) out += c;"));
}

void TestOnlinePageWording() {
  CHECK(std::string_view(kOnlineEnabledLabel) == "Online play");
  CHECK(std::string_view(kOnlineEnabledHelp) ==
        "Lets the game's own online menus create and join matches with your friends. Players meet through the online "
        "service; the game itself runs directly between your PCs whenever it can.");
  CHECK(std::string_view(kOnlinePingLabel) == "Ping display");
  CHECK(std::string_view(kOnlinePingHelp) ==
        "Shows the round trip to the other player in game. F10 switches it while playing.");
  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "\"Add friends and see who is online. Play them from the game's own online menus.\""));
  const std::string page = ReadSource("online_page.cpp");
  for (const char* shown : {"\"Share this code so another player can add you.\"", "\"Friend code copied.\"",
                            "\"Testing...\" : \"Run test\"", "ui.Heading(\"Friends\");",
                            "\"Their friend code, for example ABCD-EFGH.\"", "\"XXXX-XXXX\"",
                            "\"New friend request\"", "\"     Accept\"", "\"Decline\"",
                            "\"Your request is waiting for an answer.\"", "\"Cancel request\"", "\"Remove\"",
                            "\"No friends yet. Add someone with their friend code.\""}) {
    CHECK(Has(page + ReadSource("friend_code_field.cpp"), shown));
  }
  CHECK(!Has(page, "Join Friends Room") && !Has(page, "room"));
  for (const char* source : {"launcher.cpp", "online_page.cpp", "online_setting.h", "player_names.h",
                             "online/friend_view.cpp", "online/connection_test.cpp"}) {
    const std::string text = ReadSource(source);
    CHECK(!Has(text, "Player Match"));
    CHECK(!Has(text, "Xbox LIVE") && !Has(text, "Challenge"));
  }
  CHECK(Has(ReadSource("online/protocol.cpp"), "case FriendState::kPlayerMatch: line = \"Online Match\"; break;"));
}

void TestTheMatchAssignmentDrivesTheGamesTransport() {
  const std::string bridge = ReadSource("online_bridge.cpp");
  CHECK(Has(bridge, "Online().Assignments()"));
  CHECK(Has(bridge, "PumpOnlineService();"));
  CHECK(Has(bridge, "Online().SetPeerLink(assignment.slot,"));  // the per-peer link; the measured round trip is the peer's own
  CHECK(Has(bridge, "g_mux = std::make_unique<np::LobbyMux>(GameSocket());"));
  CHECK(Has(ReadSource("netplay/lobby_mux.cpp"), "if (relay) return RelayPeerEndpoint(slot);"));
  CHECK(Has(bridge, "g_hold.Hold(g_tag, slot, datagram)"));
}

void TestGameRuntimeReconnectsWithoutTheLauncher() {
  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  const size_t skipped = app.find("if (!show_launcher_) {");
  const size_t configured = app.find("ConfigureOnline(defaults.user_data_root);", skipped);
  const size_t applied = app.find("ApplyMultisampling();", skipped);
  CHECK(skipped != std::string::npos && configured != std::string::npos && configured < applied);
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(Has(runtime, "DEBUG_SAULO online runtime reconnect begin"));
  CHECK(Has(runtime, "kLockWaitSeconds"));
  CHECK(Has(ReadSource("window_host.cpp"), "tmnt::PumpOnlineNetwork(base);"));
  CHECK(Has(app, "tmnt::StopOnline();"));
  CHECK(Has(runtime, "if (!g_configured || g_stopped) return;"));
}

void TestPlayerMatchHooksAreBuilt() {
  const std::string cmake = ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / "CMakeLists.txt");
  for (const char* source : {"src/player_match_hook.cpp", "src/player_match_net_hook.cpp", "src/online_bridge.cpp"}) {
    CHECK(Has(cmake, source));
  }
  CHECK(!Has(ReadSource("player_match_hook.cpp"), "token"));
}

void TestOnlineOffIsTheOnlyGateForTheNetwork() {
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(Has(runtime, "settings.enabled = REXCVAR_GET(tmnt_online_enabled);"));
  const std::string service = ReadSource("online/online_service.cpp");
  CHECK(Has(service, "if (!settings_.enabled) return;"));
}

// A launcher session's settings: the store the Online page edits, and what has reached the player file at once.
struct GateRig {
  trg::MemorySettings settings;
  std::map<std::string, std::string> file;
  SaveSettingNow save_now = [this](std::string_view key, const std::string& value) {
    file[std::string(key)] = value;
    return true;
  };

  GateRig() {
    settings.true_value = "true";
    settings.false_value = "false";
  }
  void Online(bool on) { settings.SetBool(kOnlineEnabledSetting, on); }
  bool PlayBlocked(bool testing = false) const { return ConnectionTestNeeded(settings, testing); }
};

void TestPlayWaitsForAConnectionTestOnlyWithOnlinePlayOn() {
  GateRig rig;
  CHECK(!rig.PlayBlocked());  // Online off, never tested
  CHECK(!rig.PlayBlocked(true));
  rig.Online(true);
  CHECK(rig.PlayBlocked());  // never tested
  CHECK(std::string_view(kConnectionTestSetting) == "tmnt_online_connection_test");
  CHECK(OnlineUdpConfiguration() == "udp 7001-7004");
}

// The gate takes only whether the test was usable: Direct or Relay available. The report itself stays in the page.
void TestOnlyAUsableTestUnlocksPlay() {
  GateRig rig;
  rig.Online(true);
  CHECK(rig.PlayBlocked(true));  // running
  CHECK(RecordConnectionTest(rig.settings, rig.save_now, true));
  CHECK(!rig.PlayBlocked());
  CHECK(rig.file[kConnectionTestSetting] == OnlineUdpConfiguration());
  CHECK(rig.PlayBlocked(true));  // a new run blocks PLAY again until it finishes
  CHECK(!Has(ReadSource("connection_test_gate.h"), "verdict") && !Has(ReadSource("connection_test_gate.h"), "ConnectionReport"));
}

void TestAFailedTestKeepsPlayBlocked() {
  GateRig rig;
  rig.Online(true);
  CHECK(RecordConnectionTest(rig.settings, rig.save_now, false));
  CHECK(rig.PlayBlocked() && rig.file[kConnectionTestSetting].empty());
  // A later failure takes back an earlier success.
  CHECK(RecordConnectionTest(rig.settings, rig.save_now, true) && !rig.PlayBlocked());
  CHECK(RecordConnectionTest(rig.settings, rig.save_now, false) && rig.PlayBlocked());
  CHECK(rig.settings.Get(kConnectionTestSetting).empty());
}

void TestAnyChangeOfTheTestedConfigurationRequiresANewTest() {
  const std::string network = NetworkFingerprint({"192.168.1.20", "127.0.0.1", "10.0.3.7"});
  CHECK(network == "10.0.3,192.168.1");
  CHECK(NetworkFingerprint({"192.168.1.99"}) == "192.168.1");  // another host on the same subnet: same network
  CHECK(NetworkFingerprint({"192.168.7.5"}) != "192.168.1");
  const std::string base = OnlineTestConfiguration("wss://one/", network);
  GateRig rig;
  rig.Online(true);
  RecordConnectionTest(rig.settings, rig.save_now, true, base);
  CHECK(!ConnectionTestNeeded(rig.settings, false, base));
  CHECK(ConnectionTestNeeded(rig.settings, false, OnlineTestConfiguration("wss://two/", network)));
  CHECK(ConnectionTestNeeded(rig.settings, false, OnlineTestConfiguration("wss://one/", "172.16.0")));
  CHECK(Has(base, ("protocol " + std::to_string(online::kLobbyProtocol)).c_str()) && Has(base, "build 1") && Has(base, "udp 7001-7004"));
}

void TestFinishedTestSurvivesARestart() {
  GateRig first;
  first.Online(true);
  RecordConnectionTest(first.settings, first.save_now, true);
  // The next launcher reads the player file: the test was written to it at once, without Save.
  GateRig next;
  next.Online(true);
  next.settings.Set(kConnectionTestSetting, first.file[kConnectionTestSetting]);
  CHECK(!next.PlayBlocked());
  ForgetConnectionTestWhenOff(next.settings);
  CHECK(!next.PlayBlocked());
}

void TestOnlineOffForgetsTheTest() {
  GateRig rig;
  rig.Online(true);
  RecordConnectionTest(rig.settings, rig.save_now, true);
  rig.Online(false);
  ForgetConnectionTestWhenOff(rig.settings);
  CHECK(rig.settings.Get(kConnectionTestSetting).empty());
  CHECK(!rig.PlayBlocked());
  rig.Online(true);
  CHECK(rig.PlayBlocked());
  // A test that finishes while Online play is off proves nothing for the next time it is on.
  rig.Online(false);
  CHECK(!RecordConnectionTest(rig.settings, rig.save_now, true));
  rig.Online(true);
  CHECK(rig.PlayBlocked());
}

void TestOnlyTheTestedConfigurationInvalidatesTheTest() {
  GateRig rig;
  rig.Online(true);
  RecordConnectionTest(rig.settings, rig.save_now, true);
  CHECK(ConnectionTestNeeded(rig.settings, false, "udp 7101-7104"));
  for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
           {kOnlineNameSetting, "Renamed"}, {kOnlinePingSetting, "true"}, {"tmnt_show_fps", "true"},
           {"resolution_scale", "2"}, {"fullscreen", "true"}, {"mnk_mode", "true"},
           {"tmnt_show_render_resolution", "true"}}) {
    rig.settings.Set(key, value);
    ForgetConnectionTestWhenOff(rig.settings);
    CHECK(!rig.PlayBlocked());
  }
  // Friends live on the online service and the game image is not part of it either.
  const std::string gate = ReadSource("connection_test_gate.h");
  CHECK(!Has(gate, "guest") && !Has(gate, "Guest") && !Has(gate, "friend"));
}

ConnectionTestView ViewOf(const GateRig& rig, bool testing, bool windows = true) {
  const bool on = rig.settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
  return DescribeConnectionTestView(on, ConnectionTestNeeded(rig.settings, testing), testing, windows);
}

void TestRequiredBlockIsHighlightedWithAPrimaryRunTest() {
  GateRig rig;
  rig.Online(true);
  const ConnectionTestView view = ViewOf(rig, false);
  CHECK(view.highlighted && view.primary_button && view.required_text);
  CHECK(std::string_view(kConnectionTestRequiredLabel) == "Connection test required");
  CHECK(rig.PlayBlocked());
}

void TestRunningTestKeepsPlayBlockedAndTheBorder() {
  GateRig rig;
  rig.Online(true);
  const ConnectionTestView view = ViewOf(rig, true);
  CHECK(rig.PlayBlocked(true) && view.highlighted);
  CHECK(!view.primary_button && !view.required_text);
  CHECK(view.windows_note && !ViewOf(rig, true, false).windows_note);
  CHECK(!ViewOf(rig, false).windows_note);
}

// Only a usable test ends the required look; a failed one leaves it, and its report stays visible.
void TestOnlyAUsableTestEndsTheRequiredLook() {
  GateRig rig;
  rig.Online(true);
  RecordConnectionTest(rig.settings, rig.save_now, false);
  const ConnectionTestView failed = ViewOf(rig, false);
  CHECK(failed.highlighted && failed.primary_button && failed.required_text && rig.PlayBlocked());
  RecordConnectionTest(rig.settings, rig.save_now, true);
  const ConnectionTestView view = ViewOf(rig, false);
  CHECK(!view.highlighted && !view.primary_button && !view.required_text);
  CHECK(!rig.PlayBlocked());
  const std::string gate = ReadSource("connection_test_gate.h");
  CHECK(!Has(gate, "verdict") && !Has(gate, "ConnectionReport"));
  CHECK(Has(ReadSource("online_page.cpp"), "if (!state.test || state.testing) return;"));  // the report stays visible
}

void TestOnlineOffHasNoRequiredLook() {
  GateRig rig;
  const ConnectionTestView view = ViewOf(rig, false);
  CHECK(!view.highlighted && !view.primary_button && !view.required_text && !view.windows_note);
  CHECK(!ViewOf(rig, true).highlighted);
  const ConnectionTestView off = DescribeConnectionTestView(false, true, true, true);
  CHECK(!off.highlighted && !off.windows_note);
}

void TestReloadKeepsTheCompletionAndOnlineOffRestoresTheRequirement() {
  GateRig first;
  first.Online(true);
  RecordConnectionTest(first.settings, first.save_now, true);
  GateRig reloaded;
  reloaded.Online(true);
  reloaded.settings.Set(kConnectionTestSetting, first.file[kConnectionTestSetting]);
  CHECK(!ViewOf(reloaded, false).highlighted && !reloaded.PlayBlocked());
  reloaded.Online(false);
  ForgetConnectionTestWhenOff(reloaded.settings);
  reloaded.Online(true);
  CHECK(ViewOf(reloaded, false).highlighted && reloaded.PlayBlocked());
  for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
           {kOnlineNameSetting, "Renamed"}, {kOnlinePingSetting, "true"}, {"tmnt_show_fps", "true"}}) {
    GateRig rig;
    rig.Online(true);
    RecordConnectionTest(rig.settings, rig.save_now, true);
    rig.settings.Set(key, value);
    CHECK(!ViewOf(rig, false).highlighted);
  }
}

void TestPlayPressIsTheSecondAskInAFrame() {
  PlayAttemptDetector play;
  CHECK(!play.PressedThisCall(10));  // the footer drawing the Play button
  CHECK(play.PressedThisCall(10));   // the press, in the same frame
  CHECK(!play.PressedThisCall(11));
  CHECK(!play.PressedThisCall(12));
  CHECK(!play.PressedThisCall(13));
  CHECK(play.PressedThisCall(13));
}

void TestAttentionScrollsFocusesAndPulsesBriefly() {
  ConnectionTestAttention attention;
  const auto idle = attention.Frame(100.0, true);
  CHECK(!idle.scroll_into_view && !idle.focus_run_test && idle.strength == 0.0f && !attention.active());

  attention.Request();
  CHECK(attention.active());
  const auto first = attention.Frame(100.0, true);
  CHECK(first.scroll_into_view && first.focus_run_test && first.strength > 0.99f);

  // Focus is asked for once, scrolling only while the page may still be settling; ordinary frames restart nothing.
  for (double t = 100.016; t < 100.35; t += 0.016) {
    const auto step = attention.Frame(t, true);
    CHECK(!step.focus_run_test && step.scroll_into_view);
  }
  const auto later = attention.Frame(100.9, true);
  CHECK(!later.scroll_into_view && !later.focus_run_test && later.strength > 0.0f && attention.active());

  // It fades out within the 2-3 s window and stays out.
  float peak = 0.0f;
  for (double t = 101.0; t < 102.5; t += 0.016) peak = std::max(peak, attention.Frame(t, true).strength);
  CHECK(peak < 0.7f);
  CHECK(attention.Frame(102.6, true).strength == 0.0f && !attention.active());
  CHECK(attention.Frame(110.0, true).strength == 0.0f);

  // A second refused Play restarts it.
  attention.Request();
  const auto again = attention.Frame(120.0, true);
  CHECK(again.focus_run_test && again.scroll_into_view && again.strength > 0.99f);
  for (double t = 120.0; t < 122.5; t += 0.05) {
    const float strength = attention.Frame(t, true).strength;
    CHECK(strength >= 0.0f && strength <= 1.0f);
  }
}

void TestAttentionNeverOutlivesTheRequirement() {
  ConnectionTestAttention attention;
  attention.Request();
  CHECK(attention.Frame(5.0, false).strength == 0.0f && !attention.active());  // Online off, or already finished
  CHECK(!attention.Frame(5.1, true).focus_run_test);
  attention.Request();
  attention.Frame(6.0, true);
  CHECK(attention.Frame(6.5, false).strength == 0.0f && !attention.active());  // the test finished mid-pulse
  CHECK(attention.Frame(6.6, true).strength == 0.0f);
}

void TestLauncherWiresPlayRefusalToTheOnlinePage() {
  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "const bool pressed = play_attempt_.PressedThisCall(ImGui::GetFrameCount());"));
  CHECK(Has(launcher, "if (pressed) online_page_.RequestConnectionTestAttention();\n        return trg::PlayCheck{false, "
                      "kConnectionTestRequired, online_page_index_};"));
  CHECK(Has(launcher, "ImGui::TextWrapped(\"%s\", kConnectionTestRequired);"));  // the Play page keeps its line
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "ui.ButtonRow(\"Connection test\", kTestHelp, state.testing ? \"Testing...\" : \"Run test\", "
                  "view.primary_button)"));
  CHECK(Has(page, "if (focus_run_test) ImGui::SetKeyboardFocusHere();"));
  CHECK(Has(page, "ImGuiChildFlags_NavFlattened"));
  CHECK(Has(page, "DrawConnectionTestBody(ui, state, view, step.focus_run_test);\n  ui.EndRows();\n  ImGui::EndChild();"));
  CHECK(Has(page, "ImGui::SetScrollY("));
  CHECK(Has(page, "t.accent.x") && !Has(page, "IM_COL32"));  // the theme's accent, no hardcoded colour
}

void TestWindowsFirewallNoteIsWindowsOnly() {
  const char* note = ConnectionTestRunningNote(true);
  CHECK(note && Has(note, "If Windows asks whether to allow network access, choose Allow access"));
  CHECK(ConnectionTestRunningNote(false) == nullptr);
#if defined(_WIN32)
  CHECK(kOnWindows);
#else
  CHECK(!kOnWindows);
#endif
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "if (view.windows_note) {\n    if (const char* note = ConnectionTestRunningNote(true)) ui.Help(note);"));
  CHECK(Has(page, "online_on, ConnectionTestNeeded(ui.settings(), state.testing, TestConfiguration()), state.testing, kOnWindows);"));
  CHECK(!Has(ReadSource("launcher.cpp"), "Allow access"));
}

void TestLauncherGatesPlayOnTheConnectionTest() {
  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "if (online_page_.PlayNeedsTest(settings_)) {\n        if (pressed)"));
  CHECK(Has(launcher, "if (game_files_ == GameFiles::kReady && online_page_.PlayNeedsTest(settings_)) {"));
  CHECK(Has(launcher, "return settings_.SaveNow({{std::string(key), value}});"));
  CHECK(!Has(launcher, "TestConnection"));  // PLAY never starts a test itself
  const std::string page = ReadSource("online_page.cpp");
  CHECK(Has(page, "ForgetConnectionTestWhenOff(settings);"));
  CHECK(Has(page, "state.tests_finished != recorded_tests_"));
  CHECK(Has(page, "if (RecordConnectionTest(settings, save_now_, usable, TestConfiguration())) {"));
  CHECK(Has(page, "const bool usable = state.test && state.test->overall_usable;"));
  CHECK(Has(page, "return ConnectionTestNeeded(settings, Online().snapshot().testing, TestConfiguration());"));
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "      kConnectionTestSetting,\n"));
  CHECK(Has(settings, "file.Set(key, TomlValue(key, value));\n  }\n  if (!file.Save()) {\n    REXLOG_ERROR(\"Cannot write "
                      "player settings to {}\", Utf8(player_file_.filename()));\n    return false;\n  }\n  return true;"));
  CHECK(Has(ReadSource("online_runtime.cpp"), "REXCVAR_DEFINE_STRING(tmnt_online_connection_test, \"\", \"TMNT\","));
}

// The gate is launcher-only: the game's own socket, bound after PLAY, keeps its lifecycle.
void TestGameplaySocketLifecycleIsUntouched() {
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(Has(runtime, "if (settings.enabled) EnsureOnlineGameSocket();"));
  CHECK(Has(runtime, "g_game_socket.Open({kGameplayPorts[0], kGameplayPorts[1], kGameplayPorts[2], kGameplayPorts[3], 0})"));
  CHECK(!Has(runtime, "kConnectionTestSetting") && !Has(runtime, "ConnectionTestNeeded"));
  const std::string test = ReadSource("online/connection_test.cpp");
  CHECK(!Has(test, "OnlineGameSocket") && Has(test, "if (handle != net::kNoSocket) net::CloseSocket(handle);"));
}

constexpr char kTestedConfiguration[] =
    "udp 7001-7004; service wss://tmnt-lobby.saulobenigno.workers.dev/; protocol 4; build 1; network 192.168.68";

std::optional<toml::table> ParsedToml(const std::string& text) {
  try {
    return toml::parse(text);
  } catch (const toml::parse_error&) {
    return std::nullopt;
  }
}

// The string settings the way GameSettings writes them: only registered string settings are quoted.
void SaveString(const std::string& path, const std::string& key, const std::string& value) {
  trg::TextFileSettings file(path);
  file.Set(key, TomlString(value));
  CHECK(file.Save());
}

// A launcher session of one instance, as tools/online-instance.ps1 leaves the file and the launcher then changes it:
// Online play on, a Connection test whose result reaches the file at once, and the save behind Play.
struct InstanceFile {
  std::filesystem::path dir;
  std::string path;

  explicit InstanceFile(const std::string& name) {
    dir = std::filesystem::temp_directory_path() / ("tmnt_online_instance_toml_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    path = (dir / "settings.toml").string();
    std::ofstream(path) << "resolution_scale = 2\ntmnt_aspect_ratio = \"16:9\"\ntmnt_online_name = \"Test-" << name << "\"\n";
  }
  ~InstanceFile() { std::filesystem::remove_all(dir); }

  std::string Text() const { return ReadFile(path); }
  std::optional<toml::table> Parsed() const { return ParsedToml(Text()); }

  void TurnOnlineOn() {
    trg::TextFileSettings file(path);
    file.Set(kOnlineEnabledSetting, "true");
    file.Set(kOnlinePingSetting, "true");
    CHECK(file.Save());
  }
};

void CheckSavedForRelaunch(const InstanceFile& instance, const std::string& name, const std::string& test) {
  const std::optional<toml::table> table = instance.Parsed();
  CHECK(table.has_value());
  if (!table) return;
  CHECK((*table)[kOnlineEnabledSetting].value_or(false));
  CHECK((*table)[kOnlineNameSetting].value_or(std::string()) == name);
  CHECK((*table)[kConnectionTestSetting].value_or(std::string()) == test);
  CHECK((*table)["resolution_scale"].value_or(0) == 2);
}

// The defect itself: a bare value parses in the launcher's line reader and fails the whole file in the game's.
void TestABareConnectionTestRejectsTheWholeFile() {
  InstanceFile instance("bare");
  instance.TurnOnlineOn();
  trg::TextFileSettings file(instance.path);
  file.Set(kConnectionTestSetting, kTestedConfiguration);
  CHECK(file.Save());
  CHECK(trg::TextFileSettings(instance.path).Get(kConnectionTestSetting) == kTestedConfiguration);
  CHECK(!instance.Parsed().has_value());
  trg::TextFileSettings(instance.path).Set(kConnectionTestSetting, "");
  std::ofstream(instance.path, std::ios::app) << kOnlineNameSetting << " = \n";
  CHECK(!instance.Parsed().has_value());
}

void TestInstanceSettingsStayValidThroughTheLauncher() {
  for (const char* name : {"a", "b"}) {
    InstanceFile instance(name);
    const std::string online_name = std::string("Test-") + name;
    CHECK(instance.Parsed().has_value());
    instance.TurnOnlineOn();
    CHECK(instance.Parsed().has_value());
    // The first test, its repeat with the value already saved, and the save behind Play, which then has nothing to write.
    SaveString(instance.path, kConnectionTestSetting, kTestedConfiguration);
    CheckSavedForRelaunch(instance, online_name, kTestedConfiguration);
    SaveString(instance.path, kConnectionTestSetting, kTestedConfiguration);
    CheckSavedForRelaunch(instance, online_name, kTestedConfiguration);
    // A test that was not usable clears the value.
    SaveString(instance.path, kConnectionTestSetting, "");
    CheckSavedForRelaunch(instance, online_name, "");
    SaveString(instance.path, kConnectionTestSetting, kTestedConfiguration);
    SaveString(instance.path, kOnlineNameSetting, "Saulo-" + std::string(name));
    CheckSavedForRelaunch(instance, "Saulo-" + std::string(name), kTestedConfiguration);
    // What the restarted process finds is what it writes back.
    const std::string once = instance.Text();
    trg::TextFileSettings reread(instance.path);
    CHECK(reread.Save());
    CHECK(instance.Text() == once);
  }
}

void TestPlayIsOpenAfterRelaunchWithoutAnotherTest() {
  InstanceFile instance("relaunch");
  instance.TurnOnlineOn();
  SaveString(instance.path, kConnectionTestSetting, kTestedConfiguration);
  const std::optional<toml::table> table = instance.Parsed();
  CHECK(table.has_value());
  trg::MemorySettings restarted;
  restarted.true_value = "true";
  restarted.false_value = "false";
  restarted.SetBool(kOnlineEnabledSetting, true);
  restarted.Set(kConnectionTestSetting, table ? (*table)[kConnectionTestSetting].value_or(std::string()) : std::string());
  CHECK(!ConnectionTestNeeded(restarted, false, kTestedConfiguration));
  CHECK(ConnectionTestNeeded(restarted, false, "udp 7001-7004; service wss://other/; protocol 4; build 1; network 192.168.68"));
}

// Files the earlier launcher already left behind load again, with their test still counted.
void TestBareValuesAlreadyWrittenAreRepaired() {
  const std::string key = kConnectionTestSetting;
  const std::string broken = "# Set from the launcher\r\n" + key + " = " + kTestedConfiguration + "\r\n\r\ntmnt_online_enabled = true\r\n";
  CHECK(!ParsedToml(broken).has_value());
  std::size_t repaired = 0;
  const std::string fixed = RepairBareString(broken, key, repaired);
  CHECK(repaired == 1);
  CHECK(Has(fixed, (key + " = \"" + kTestedConfiguration + "\"\r\n").c_str()));
  const std::optional<toml::table> table = ParsedToml(fixed);
  CHECK(table.has_value());
  if (table) CHECK((*table)[key].value_or(std::string()) == kTestedConfiguration && (*table)[kOnlineEnabledSetting].value_or(false));
  repaired = 0;
  CHECK(RepairBareString(fixed, key, repaired) == fixed && repaired == 0);

  const std::string empty = key + " = \ntmnt_online_enabled = true\n";
  CHECK(!ParsedToml(empty).has_value());
  const std::string filled = RepairBareString(empty, key, repaired);
  CHECK(repaired == 1 && filled == key + " = \"\"\ntmnt_online_enabled = true\n");
  CHECK(ParsedToml(filled).has_value());

  repaired = 0;
  const std::string other = "# " + key + " = a b\n" + key + "_x = 1\nname = \"x\"";
  CHECK(RepairBareString(other, key, repaired) == other && repaired == 0);
  CHECK(TomlString("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(ParsedToml("x = " + TomlString("a\"b\\c")).has_value());
}

void TestEveryLauncherWriteQuotesStrings() {
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "file.Set(key, TomlValue(key, value));\n  }\n  if (!file.Save()) {\n    REXLOG_ERROR(\"Cannot write "
                      "player settings to {}\", Utf8(player_file_.filename()));\n    return false;\n  }\n  saved_ = pending_;"));
  CHECK(Has(settings, "file.Set(key, TomlValue(key, value));\n  }\n  if (!file.Save()) {\n    REXLOG_ERROR(\"Cannot write "
                      "player settings to {}\", Utf8(player_file_.filename()));\n    return false;\n  }\n  return true;"));
  CHECK(!Has(settings, "file.Set(key, value);"));
  CHECK(Has(settings, "RepairBareString(RepairLegacyToggles(text, repaired), kConnectionTestSetting, repaired)"));
}

// Play relaunches into the same user folder and log; the new process signs in by itself on the production service.
void TestRelaunchKeepsTheInstance() {
  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::RelaunchSelf(L\"--tmnt_skip_launcher=true\")"));
  CHECK(Has(app, "tmnt::ConfigureOnline(defaults.user_data_root);"));
  const std::string script = ReadFile(std::filesystem::path(TMNT_SOURCE_DIR) / "tools" / "online-instance.ps1");
  CHECK(Has(script, "--user_data_root=") && Has(script, "Remove-Item Env:TMNT_LOBBY"));
  CHECK(!Has(script, "online_identity.ini') -Destination") && !Has(script, "Copy-Item"));
  CHECK(Has(ReadSource("online_runtime.cpp"), "return online::LoadOrCreateIdentity(g_user_dir);"));
}

void TestDefaultServiceAddressAndVersionSource() {
  const std::string service = ReadSource("online/online_service.h");
  CHECK(Has(service, "wss://tmnt-lobby.saulobenigno.workers.dev/"));
  CHECK(!Has(service, "dcp-lobby"));
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(Has(runtime, "config.version = PortVersion();"));
  CHECK(!Has(runtime, "0.2.2"));
}

}  // namespace

int main() {
  TestCornerReadoutsStackWithoutOverlap();
  TestNoDebugOverlayText();
  TestLocalAndOnlineNamesAreSeparate();
  TestOnlineNameFieldMatchesItsHelp();
  TestOnlinePageWording();
  TestOnlineDefaultsToOff();
  TestOnlineNamePersists();
  TestSettingsReachTheRuntime();
  TestNoSettingIsLockedWhileOnline();
  TestPingDisplayRules();
  TestOnlinePageSections();
  TestLauncherHasAnOnlinePageWithoutTechnicalControls();
  TestFriendCodeCopyAndPasteUseTheNativeClipboard();
  TestConnectionTestPageShowsTheReport();
  TestTheMatchAssignmentDrivesTheGamesTransport();
  TestGameRuntimeReconnectsWithoutTheLauncher();
  TestPlayerMatchHooksAreBuilt();
  TestOnlineOffIsTheOnlyGateForTheNetwork();
  TestDefaultServiceAddressAndVersionSource();
  TestABareConnectionTestRejectsTheWholeFile();
  TestInstanceSettingsStayValidThroughTheLauncher();
  TestPlayIsOpenAfterRelaunchWithoutAnotherTest();
  TestBareValuesAlreadyWrittenAreRepaired();
  TestEveryLauncherWriteQuotesStrings();
  TestRelaunchKeepsTheInstance();
  TestPlayWaitsForAConnectionTestOnlyWithOnlinePlayOn();
  TestOnlyAUsableTestUnlocksPlay();
  TestAFailedTestKeepsPlayBlocked();
  TestAnyChangeOfTheTestedConfigurationRequiresANewTest();
  TestFinishedTestSurvivesARestart();
  TestOnlineOffForgetsTheTest();
  TestOnlyTheTestedConfigurationInvalidatesTheTest();
  TestRequiredBlockIsHighlightedWithAPrimaryRunTest();
  TestRunningTestKeepsPlayBlockedAndTheBorder();
  TestOnlyAUsableTestEndsTheRequiredLook();
  TestOnlineOffHasNoRequiredLook();
  TestReloadKeepsTheCompletionAndOnlineOffRestoresTheRequirement();
  TestPlayPressIsTheSecondAskInAFrame();
  TestAttentionScrollsFocusesAndPulsesBriefly();
  TestAttentionNeverOutlivesTheRequirement();
  TestLauncherWiresPlayRefusalToTheOnlinePage();
  TestWindowsFirewallNoteIsWindowsOnly();
  TestLauncherGatesPlayOnTheConnectionTest();
  TestGameplaySocketLifecycleIsUntouched();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
