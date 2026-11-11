// Detailed logging: what stays in a normal log and what needs Detailed logging, against the lines the game writes, the
// saved choice and the wiring of the setting. The real sink and session lifecycle are in tmnt_log_filter_tests.
// Run: tmnt_log_policy_tests

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "log_policy.h"
#include "trg/settings.h"

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
  std::ifstream in(std::string(TMNT_ROOT_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

bool Keep(std::string_view message) {
  return tmnt::KeepWithoutDetailedLogging(tmnt::LogSeverity::kInfo, message);
}

void TestNormalLog() {
  std::printf("normal log\n");
  constexpr std::string_view lines[] = {
      "Mapped guest memory at 0x100000000",
  };
  CHECK(!Keep(lines[0]));
  constexpr std::string_view kept[] = {
      "Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled port 0.1.0",
      "Build: RelWithDebInfo x86-64 | ReXGlue SDK 0.10.0 | Direct3D 12",
      "Game files: valid (default.xex, size 4808704, CRC32 FC04059D)",
      "Graphics card: NVIDIA GeForce RTX 4070 (adapter 0)",
      "Launcher Graphics: Graphics card=Automatic",
      "Runtime renderer: render=2560x1440 | guest=1280x720 | msaa=2x native",
      "Scheduling: 1 ms sleep took 1.7 ms, now 1.9 ms (timer 1 ms, power throttling off yes)",
      "Updater: Update check failed: The server answered 404.",
      "Window closing, hard-exiting process.",
      "Execution complete: clean exit, uptime 00:10:00",
      "online worker connected",
      "online signed in name=Player code=4GPA-C7UH",
      "online worker disconnected reason=closed",
      "online runtime LIVE state active xuid=000980D4FCA1BE1D",
      "online connection test verdict",
      "party joined slot=1 diag=635908e8f402 members=1",
      "party ended reason=left",
      "room joined generation=1 relay=true members=1",
      "room disconnected reason=closed",
      "transport selected peer=1 selected=direct relay_available=true",
      "transport summary peer=1 relay_available=true",
      "transport lost peer=1",
      "relay resumed peer=2",
      "relay suspended peer=2 reason=gone",
      "peer left peer=3 reason=closed",
      "direct path not established peer=1 reason=timeout",
      "udp rendezvous failed peer=1",
      "udp socket bound local=0.0.0.0:7001 phase=game",
      "udp socket bind failed",
      "player_match lobby transport begin tag=0000000A generation=1",
      "player_match peer routed slot=1 transport=direct held=0",
      "player_match session left reason=quit",
      "player_match join refused session=1",
      "invite delivered generation=1 session=1 host=0",
  };
  for (const std::string_view line : kept) {
    CHECK(Keep(line));
  }
}

void TestDetailedOnly() {
  std::printf("Detailed logging only\n");
  constexpr std::string_view dropped[] = {
      "Saved music level: 4",
      "Skip intro: startup goes to the title screen",
      "Menu movies: 4 patched in a bundle of 19784989 bytes",
      "[Stats] Local stats file loaded",
      "[Live] local profile accepted in menu (controller 0)",
      "[Achievement] Local achievements menu bridge installed",
      "Initializing shader storage for title 584109D2...",
      "Translated 119 shaders from the storage in 3 milliseconds",
      "Created 49370 graphics pipelines (not including reading the descriptions) from the storage in 121 milliseconds",
      "SetInterruptCallback(82129B30, 4000E300)",
      "Function table initialized for module: code=87520000-876DEE88, image=87500000-876F0000",
      "Registering module: tmnt_r.xex (code_base=87520000)",
      "SDL OnControllerDeviceAdded: connection order 0, device 1.",
      "audio endpoint 'Speaker Out': 2 ch, 48000 Hz, format 0x8120; submitting 2 ch",
      "stun candidate gathered",
      "lobby heartbeat swaps=4510",
      "CreateMatchStep UpdateWaitDefered ENTER",
      "search-time pool free ptr=0",
      "FrameTrace ENTER frame=1",
  };
  for (const std::string_view line : dropped) {
    CHECK(!Keep(line));
  }
  CHECK(!tmnt::KeepWithoutDetailedLogging(tmnt::LogSeverity::kBelowInfo, "online worker connected"));
}

void TestFailuresSurvive() {
  std::printf("warnings, errors and crash reports stay in both modes\n");
  constexpr std::string_view lines[] = {
      "Not a listed line",
      "============================================================",
      "CRASH REPORT",
      "Windows unhandled exception",
      "Stack trace: 12 frames",
      "Failed to create a Direct3D 12 device",
  };
  for (const std::string_view line : lines) {
    CHECK(tmnt::KeepWithoutDetailedLogging(tmnt::LogSeverity::kWarningOrAbove, line));
  }
}

void TestSavedChoice() {
  std::printf("a fresh profile is On, a saved choice is kept\n");
  const std::filesystem::path file = std::filesystem::temp_directory_path() / "tmnt_detailed_logging_test.toml";
  std::filesystem::remove(file);
  {
    std::ofstream(file) << "# Turtles in Time Re-Shelled player settings\nresolution_scale = 2\n";
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(!settings.Has("detailed_logging"));
    CHECK(settings.GetBool("detailed_logging", true));
    settings.Set("detailed_logging", "false");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.Has("detailed_logging"));
    CHECK(!settings.GetBool("detailed_logging", true));
    CHECK(settings.GetInt("resolution_scale", 1) == 2);
    settings.Set("detailed_logging", "true");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.GetBool("detailed_logging", false));
    settings.Set("detailed_logging", "false");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(!settings.GetBool("detailed_logging", true));
  }
  std::filesystem::remove(file);
}

void TestWiring() {
  std::printf("default, persistence, About page\n");
  const std::string filter = ReadFile("src/log_filter.cpp");
  CHECK(Has(filter, "REXCVAR_DEFINE_BOOL(detailed_logging, true,"));
  CHECK(Has(filter, "REXCVAR_DEFINE_BOOL(tmnt_discard_normal_logs, false,"));
  CHECK(Has(filter, "GetFlagByName(\"detailed_logging\") != \"false\""));
  CHECK(Has(filter, "GetFlagByName(\"tmnt_discard_normal_logs\") == \"true\""));

  const std::string settings = ReadFile("src/settings.cpp");
  CHECK(Has(settings, "\"detailed_logging\","));

  const std::string launcher = ReadFile("src/launcher.cpp");
  const size_t toggle = launcher.find("ui.Toggle(\"Detailed logging\"");
  const size_t folder = launcher.find("\"Open logs folder\"");
  CHECK(toggle != std::string::npos && folder != std::string::npos && toggle < folder);
  CHECK(Has(launcher, "\"detailed_logging\", true"));
  CHECK(Has(launcher, "settings_.Get(\"detailed_logging\", \"true\")"));
  CHECK(Has(launcher, "trg::OpenInFileManager(Utf8(folder))"));

  const std::string session = ReadFile("src/session_log.cpp");
  CHECK(Has(session, "!DiscardNormalLogs()"));

  for (const char* file : {"src/log_filter.cpp", "src/log_policy.h", "src/settings.cpp", "src/launcher.cpp"}) {
    CHECK(!Has(ReadFile(file), "DEBUG_SAULO"));
  }
}

void TestPortuguese() {
  std::printf("pt-BR rows\n");
  const std::string table = ReadFile("src/localization/pt_br_ui_text.cpp");
  CHECK(Has(table, "Detailed logging\tLog detalhado"));
  CHECK(Has(table, "Open logs folder\tAbrir pasta de logs"));
  CHECK(Has(table, "Logs folder\tPasta de logs"));
  CHECK(Has(table, "On keeps the full log, for reporting a problem."));
  CHECK(Has(table, "Ligado guarda o log completo"));
  CHECK(!Has(table, "Keeps logs for normal sessions"));
}

// Log level of every DEBUG_SAULO call in a patch: "INFO", "WARN" or "ERROR".
std::vector<std::string> DebugSauloLogLevels(const std::string& patch) {
  std::vector<std::string> levels;
  for (size_t at = patch.find("REXGPU_"); at != std::string::npos; at = patch.find("REXGPU_", at + 1)) {
    const size_t open = patch.find('(', at);
    if (open == std::string::npos || open - at > 16) {
      continue;
    }
    size_t i = open + 1;
    while (i < patch.size() && std::string_view(" \t\r\n+").find(patch[i]) != std::string_view::npos) {
      ++i;
    }
    if (patch.compare(i, 12, "\"DEBUG_SAULO") == 0) {
      levels.push_back(patch.substr(at + 7, open - at - 7));
    }
  }
  return levels;
}

// What the compact log keeps for a line logged at this level (Detailed logging Off).
bool SurvivesCompactLog(const std::string& level) {
  const tmnt::LogSeverity severity = level == "WARN" || level == "ERROR" ? tmnt::LogSeverity::kWarningOrAbove
                                     : level == "INFO"                  ? tmnt::LogSeverity::kInfo
                                                                        : tmnt::LogSeverity::kBelowInfo;
  return tmnt::KeepWithoutDetailedLogging(severity, "DEBUG_SAULO");
}

void TestTemporaryDiagnostics() {
  std::printf("temporary diagnostics in compact logs\n");
  for (const char* patch : {"patches/rexglue-sdk/0092-debug-saulo-vulkan-draw-telemetry.linux.patch",
                            "patches/rexglue-sdk/0093-debug-saulo-issue2-d3d12-telemetry.windows.patch"}) {
    const std::vector<std::string> levels = DebugSauloLogLevels(ReadFile(patch));
    CHECK(!levels.empty());
    for (const std::string& level : levels) {
      CHECK(SurvivesCompactLog(level));
    }
  }
}

}  // namespace

int main() {
  TestNormalLog();
  TestDetailedOnly();
  TestFailuresSurvive();
  TestSavedChoice();
  TestWiring();
  TestPortuguese();
  TestTemporaryDiagnostics();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
