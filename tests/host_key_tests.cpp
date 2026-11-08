// Host keys: ReXGlue's F3 and F4 overlays removed through its own binds, F3 owned by the Ping display, gameplay bindings
// unable to take either key, and the Ping readout's text, visibility and layout. Headless: no window, no runtime.
// Run: tmnt_host_key_tests

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

#include "classic_soundtrack_overlay.h"
#include "corner_overlay.h"
#include "keybinds.h"
#include "ping_display.h"

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

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(TMNT_ROOT_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

void TestF9Ownership() {
  std::printf("F9: the SNES soundtrack switch is the only owner, gameplay cannot take it, documented\n");
  CHECK(std::string_view(tmnt::kClassicSoundtrackKey) == "F9");
  CHECK(tmnt::KeyboardBindingIsReserved(ImGuiKey_F9));
  CHECK(tmnt::KeyboardBindingFor(ImGuiKey_F9, false, false, false).empty());
  CHECK(tmnt::KeyboardBindingFor(ImGuiKey_F9, true, false, false).empty());
  CHECK(std::string_view(tmnt::kPingDisplayKey) != tmnt::kClassicSoundtrackKey);

  const std::filesystem::path src = std::filesystem::path(TMNT_ROOT_DIR) / "src";
  std::size_t owners = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
    const std::string name = entry.path().filename().string();
    if (!entry.is_regular_file() || (entry.path().extension() != ".cpp" && entry.path().extension() != ".h")) continue;
    const std::string text = ReadFile("src/" + std::filesystem::relative(entry.path(), src).generic_string());
    if (name == "keybinds.h") continue;
    owners += Has(text, "\"F9\"") ? 1 : 0;
  }
  CHECK(owners == 1);
  CHECK(Has(ReadFile("src/classic_soundtrack_overlay.cpp"), "rex::ui::RegisterBind(\"bind_tmnt_classic_soundtrack\", kClassicSoundtrackKey,"));
  CHECK(Has(ReadFile("docs/input.md"), "**F9**") && Has(ReadFile("docs/launcher.md"), "F7, F9, F10"));
  CHECK(Has(ReadFile("docs/classic-soundtrack.md"), "**F9**"));
}

void TestSdkBindsRemoved() {
  std::printf("SDK overlays: unregistered in OnCreateDialogs, not hidden\n");
  const std::string counter = ReadFile("src/frame_counter.h");
  CHECK(Has(counter, "kRemovedSdkBinds[] = {\"bind_debug_overlay\", \"bind_settings\"};"));
  const std::string app = ReadFile("src/tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {"));
  CHECK(Has(app, "for (const char* bind : tmnt::kRemovedSdkBinds) rex::ui::UnregisterBind(bind);"));
  CHECK(Has(app, "#include <rex/ui/keybinds.h>"));

  // The names and keys the SDK really registers; its dispatch skips an unregistered bind, so F3 reaches the Ping bind.
  const std::string sdk = ReadFile("tools/rexglue/win-amd64/share/rexglue/rex_app.cpp");
  if (sdk.empty()) {
    std::printf("  skipped: the pinned SDK is not unpacked in tools/rexglue\n");
    return;
  }
  const size_t debug = sdk.find("RegisterBind(\"bind_debug_overlay\", \"F3\"");
  const size_t settings = sdk.find("RegisterBind(\"bind_settings\", \"F4\"");
  const size_t dialogs = sdk.find("OnCreateDialogs(imgui_drawer_.get());");
  CHECK(debug != std::string::npos && settings != std::string::npos && dialogs != std::string::npos);
  CHECK(debug < dialogs && settings < dialogs);
}

void TestF3Ownership() {
  std::printf("F3: the Ping display's, nothing in the port binds F4\n");
  CHECK(std::string_view(tmnt::kPingDisplayKey) == "F3");
  const std::string ping = ReadFile("src/ping_display.cpp");
  CHECK(Has(ping, "RegisterBind(\"bind_tmnt_ping_display\", kPingDisplayKey,"));
  CHECK(Has(ReadFile("src/tmnt_turtles_in_time_reshelled_app.h"), "tmnt::CreatePingDisplay(imgui_drawer());"));
  const std::regex fixed_key(R"(RegisterBind\([^,]+,\s*"F[34]")");
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(TMNT_ROOT_DIR) / "src")) {
    if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
    CHECK(!std::regex_search(ReadFile("src/" + entry.path().filename().string()), fixed_key));
  }
}

void TestGameplayCannotTakeTheKeys() {
  std::printf("reserved: gameplay bindings refuse F3 and F4, with modifiers too\n");
  for (ImGuiKey key : {ImGuiKey_F3, ImGuiKey_F4}) {
    CHECK(tmnt::KeyboardBindingIsReserved(key));
    CHECK(tmnt::KeyboardBindingFor(key, false, false, false).empty());
    CHECK(tmnt::KeyboardBindingFor(key, true, false, false).empty());
  }
  CHECK(!tmnt::KeyboardBindingIsValid("F3"));
  CHECK(!tmnt::KeyboardBindingIsValid("Shift+F4"));
}

void TestPingText() {
  std::printf("ping: numeric, unmeasured and unconnected text\n");
  using tmnt::MeasuredRtt;
  using tmnt::PingText;
  CHECK(PingText(0) == "Ping: 0 ms");
  CHECK(PingText(32) == "Ping: 32 ms");
  CHECK(PingText(1200) == "Ping: 1200 ms");
  CHECK(PingText(std::nullopt) == "Ping: --");
  CHECK(PingText(MeasuredRtt(tmnt::online::NetplayLink::kConnected, std::nullopt)) == "Ping: --");
  CHECK(PingText(MeasuredRtt(tmnt::online::NetplayLink::kNone, 40)) == "Ping: --");
  CHECK(PingText(MeasuredRtt(tmnt::online::NetplayLink::kConnected, 42)) == "Ping: 42 ms");
}

void TestPingVisibility() {
  std::printf("ping: hidden while off, without Online play it stays hidden\n");
  CHECK(!tmnt::PingVisible(true, false));
  CHECK(!tmnt::PingVisible(false, true));
  CHECK(!tmnt::PingVisible(false, false));
  CHECK(tmnt::PingVisible(true, true));
  CHECK(Has(ReadFile("src/online_setting.h"), "kDefaultOnlinePing = false;"));
  CHECK(Has(ReadFile("src/settings.cpp"), "kOnlinePingSetting,"));  // persisted as in SF2
}

void TestPingLayout() {
  std::printf("ping: same panel as the FPS counter, underneath it when both are shown\n");
  CHECK(tmnt::FrameCounterSlot() == 0);
  CHECK(tmnt::PingSlot(false) == 0);
  CHECK(tmnt::PingSlot(true) == 1);
  const tmnt::CornerPanel counter = tmnt::CornerPanelAt(1920, 16, 120, 16, tmnt::FrameCounterSlot());
  const tmnt::CornerPanel ping = tmnt::CornerPanelAt(1920, 16, 90, 16, tmnt::PingSlot(true));
  const tmnt::CornerPanel alone = tmnt::CornerPanelAt(1920, 16, 90, 16, tmnt::PingSlot(false));
  CHECK(ping.y0 >= counter.y1);
  CHECK(ping.x1 == counter.x1);
  CHECK(alone.y0 == counter.y0);
  CHECK(Has(ReadFile("src/ping_display.cpp"), "DrawCornerText(PingText(Online().WorstRtt()), PingSlot(counter, ClassicSoundtrackNoticeVisible()));"));
}

void TestPingIsReadOnly() {
  std::printf("ping: reads the session's measurement, generates no traffic\n");
  const std::string ping = ReadFile("src/ping_display.cpp");
  for (const char* banned : {"Send(", "socket", "Socket", "PeerPing", "Due(", "websocket", "Fetch(", "http"}) {
    CHECK(!Has(ping, banned));
  }
  const std::string service = ReadFile("src/online/online_service.cpp");
  const size_t begin = service.find("std::optional<int> OnlineService::WorstRtt() const {");
  const size_t end = service.find("std::vector<NetplayAssignment> OnlineService::Assignments() const {");
  CHECK(begin != std::string::npos && end != std::string::npos && begin < end);
  const std::string worst = service.substr(begin, end - begin);
  CHECK(Has(worst, "session_->peers()"));
  CHECK(!Has(worst, "Send") && !Has(worst, "Pump"));

  // The measurement follows the link the match is on.
  const std::string session = ReadFile("src/online/match_session.cpp");
  CHECK(Has(session, "link.transport == MatchTransport::kRelay ? netplay::RelayPeerEndpoint(link.slot)"));
  CHECK(Has(session, "view.rtt_ms = ping_.rtt_ms(link.slot);"));
}

void TestDocs() {
  std::printf("docs: F3 is the Ping display, F4 is free\n");
  CHECK(Has(ReadFile("docs/online.md"), "**F3** shows or hides it"));
  CHECK(Has(ReadFile("docs/input.md"), "**F3** toggles the Ping display"));
  CHECK(Has(ReadFile("docs/launcher.md"), "F3 in game"));
  CHECK(!Has(ReadFile("docs/launcher.md"), "F10 in game"));
}

}  // namespace

int main() {
  TestF9Ownership();
  TestSdkBindsRemoved();
  TestF3Ownership();
  TestGameplayCannotTakeTheKeys();
  TestPingText();
  TestPingVisibility();
  TestPingLayout();
  TestPingIsReadOnly();
  TestDocs();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
