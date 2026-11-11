// Focused tests of the Bug Report page's logic: which logs it offers, what the privacy filter leaves, and that the preview
// and the request are the same report. Synthetic logs in a temporary folder; no window, no network.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "incident_json.h"
#include "incident_report.h"
#include "log_scrubber.h"
#include "report_history.h"
#include "updater/json.h"

namespace fs = std::filesystem;
using namespace tmnt;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

const std::string kGameSession =
    "[run] 2026-11-05T03:51:00\nLauncher shown\nLauncher closed, starting the game\n";
const std::string kGameplaySession =
    kGameSession + "Runtime configuration:\nRuntime OS: Windows 11\nExecution complete: clean exit, uptime 00:01:00\n";
const std::string kLauncherSession = "[run] 2026-11-05T03:51:00\nLauncher shown\nLauncher closed\n";
const std::string kLauncherCrash = "[run] 2026-11-05T03:51:00\nLauncher shown\nLauncher window 1689x840\n";

struct TempFolder {
  fs::path path;
  TempFolder() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("tmnt-incident-tests-" + std::to_string(tick));
    fs::create_directories(path);
  }
  ~TempFolder() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string LogName(int number) {
  char name[64];
  std::snprintf(name, sizeof(name), "tmnt_turtles_in_time_reshelled_%03d.log", number);
  return name;
}

fs::path WriteLog(const fs::path& folder, const std::string& name, const std::string& text, int minutes_ago) {
  const fs::path file = folder / name;
  std::ofstream(file, std::ios::binary) << text;
  fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutes_ago));
  return file;
}

std::vector<std::string> Names(const std::vector<GameLog>& logs) {
  std::vector<std::string> names;
  for (const GameLog& log : logs) names.push_back(log.name);
  return names;
}

PrivateFacts Facts() {
  PrivateFacts facts;
  facts.user_names = {"Zefa Tester", "zefa"};
  facts.machine_names = {"ZEFA-DESKTOP"};
  facts.profile_folders = {"C:\\Users\\Zefa Tester"};
  facts.install_folders = {"D:\\Jogos\\Zefa\\TMNT"};
  facts.secrets = {std::string(64, 'a'), "ABCD-EFGH"};
  return facts;
}

std::string Scrub(std::string_view text) { return ScrubText(text, Facts()); }
bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

std::string ReadSource(const char* relative) {
  std::ifstream in(fs::path(TMNT_SOURCE_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<std::string> ReadMessages(const fs::path& path) {
  std::vector<std::string> messages;
  std::ifstream in(path, std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) messages.push_back(line);
  }
  return messages;
}

ReportFacts SampleFacts() {
  return {"0.1.0", "TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)", "Windows",
          "Windows 11 (build 26200)", "ReXGlue SDK 0.10.0"};
}

std::vector<std::string> WireMessages() {
  TempFolder temp;
  const fs::path file = WriteLog(temp.path, LogName(1), "Runtime configuration:\nRuntime OS: Windows 11\n", 0);
  const GameLog log{file, LogName(1), std::time(nullptr), fs::file_size(file), LogActivity::kGameplay, LogRole::kLastGameplay};
  const std::optional<IncidentPackage> package =
      BuildIncidentPackage("The game crashes when the second player joins a match.", log, SampleFacts(), Facts());
  if (!package) return {};
  TrackedReport tracked;
  tracked.ticket = "TMNT-ABCDEF";
  tracked.receipt = std::string(64, 'a');
  return {IncidentFileMessage(1, *package), IncidentStatusMessage(2, {tracked}), IncidentReadMessage(3, tracked)};
}

void TestNewestFiveFirst() {
  TempFolder temp;
  const int ages[] = {50, 10, 40, 20, 60, 30, 70};
  for (int i = 0; i < 7; ++i) WriteLog(temp.path, LogName(i + 1), kGameSession, ages[i]);
  WriteLog(temp.path, "last_run.log", kGameSession, 1);
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_abc.log", kGameSession, 1);
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_012.log.bak", kGameSession, 1);
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_7.log", kGameSession, 1);
  const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
  const std::vector<std::string> expected = {LogName(2), LogName(4), LogName(6), LogName(3), LogName(1)};
  CHECK(logs.size() == kMostGameLogs);
  CHECK(Names(logs) == expected);
}

void TestRotationAndEmptyLogs() {
  TempFolder temp;
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_003.1.log", kGameSession, 20);
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_003.log", kGameSession, 10);
  WriteLog(temp.path, "tmnt_turtles_in_time_reshelled_004.log", "", 5);
  const std::vector<std::string> expected = {"tmnt_turtles_in_time_reshelled_003.log",
                                             "tmnt_turtles_in_time_reshelled_003.1.log"};
  CHECK(Names(FindGameLogs(temp.path, {})) == expected);
  CHECK(FindGameLogs(temp.path / "missing", {}).empty());
}

void TestSessionsThatAreNotGameLogs() {
  TempFolder temp;
  WriteLog(temp.path, LogName(1), kGameSession, 40);
  WriteLog(temp.path, LogName(2), kLauncherSession, 30);
  WriteLog(temp.path, LogName(3), kLauncherCrash, 20);
  const fs::path running = WriteLog(temp.path, LogName(4), kLauncherCrash, 0);
  const std::vector<std::string> expected = {LogName(4), LogName(3), LogName(1)};
  CHECK(Names(FindGameLogs(temp.path, running)) == expected);
}

void TestCurrentSessionAndLastGameplay() {
  using enum LogRole;
  TempFolder temp;
  WriteLog(temp.path, LogName(1), kGameplaySession, 60);
  WriteLog(temp.path, LogName(2), kGameplaySession, 50);
  WriteLog(temp.path, LogName(3), kLauncherSession, 40);
  const fs::path running = WriteLog(temp.path, LogName(4), kLauncherCrash, 120);
  const std::vector<GameLog> logs = FindGameLogs(temp.path, running);
  const std::vector<LogRole> roles = [&] {
    std::vector<LogRole> out;
    for (const GameLog& log : logs) out.push_back(log.role);
    return out;
  }();
  CHECK(Names(logs) == (std::vector<std::string>{LogName(4), LogName(2), LogName(1)}));
  CHECK(roles == (std::vector<LogRole>{kCurrentLauncher, kLastGameplay, kOtherGameplay}));
  CHECK(DefaultGameLog(logs) == 1);
}

void TestDefaultGameLog() {
  TempFolder failed;
  WriteLog(failed.path, LogName(1), kGameplaySession, 60);
  WriteLog(failed.path, LogName(2), kGameSession, 30);
  const fs::path running = WriteLog(failed.path, LogName(3), kLauncherCrash, 0);
  const std::vector<GameLog> logs = FindGameLogs(failed.path, running);
  CHECK(DefaultGameLog(logs) == 1);
  CHECK(std::string(LogRoleText(logs[2].role)) == "Last gameplay");
}

void TestCharacterBounds() {
  CHECK(CountCharacters("") == 0);
  CHECK(CountCharacters("abc") == 3);
  CHECK(CountCharacters("caf\xC3\xA9") == 4);
  const std::string text = "a\xC3\xA9\xF0\x9F\x90\xA2z";
  CHECK(BytesOfCharacters(text, 0) == 0);
  CHECK(BytesOfCharacters(text, 1) == 1);
  CHECK(BytesOfCharacters(text, 2) == 3);
  CHECK(BytesOfCharacters(text, 3) == 7);
  CHECK(BytesOfCharacters(text, 4) == text.size());
  CHECK(BytesOfCharacters(text, 99) == text.size());
  CHECK(CheckDraft("   short   ", true) == DraftProblem::kDescriptionTooShort);
  CHECK(CheckDraft("long enough", false) == DraftProblem::kNoLog);
  CHECK(CheckDraft("long enough", true) == DraftProblem::kNone);
}

void TestLastBytes() {
  CHECK(LastBytes("one\ntwo\nthree", 100) == "one\ntwo\nthree");
  CHECK(LastBytes("one\ntwo\nthree", 9) == "three");
  CHECK(LastBytes("one\ntwo\nthree\n", 9) == "three\n");
  CHECK(LastBytes("one\ntwo\nthree", 5) == "three");
  CHECK(LastBytes("abcdef", 3) == "def");
}

void TestScrubber() {
  const std::string text =
      "User data: C:\\Users\\Zefa Tester\\AppData\\Roaming\\TMNT\n"
      "Install: D:\\Jogos\\Zefa\\TMNT\\game\\user_data\\settings.toml\n"
      "Machine: ZEFA-DESKTOP user zefa token=abcdef\n"
      "Graphics: NVIDIA GeForce RTX 4070 driver 551.23\n"
      "peer 1234567890 connected 203.0.113.7\n"
      "Runtime OS: Windows 11 (build 26200)\n";
  const std::string out = Scrub(text);
  CHECK(Has(out, "<home>"));
  CHECK(Has(out, "<install>"));
  CHECK(!Has(out, "Zefa Tester"));
  CHECK(!Has(out, "ZEFA-DESKTOP"));
  CHECK(!Has(out, "203.0.113.7"));
  CHECK(Has(out, "<ipv4>"));
  CHECK(Has(out, "NVIDIA GeForce RTX 4070"));
  CHECK(Has(out, "driver 551.23"));
  CHECK(Has(out, "Windows 11 (build 26200)"));
  CHECK(Has(out, "<secret>"));
  CHECK(Has(out, "<player>"));
  CHECK(!Has(out, "abcdef"));
  const std::string bad = Scrub(std::string("ok\n\x01\xFF bad\r\n"));
  CHECK(!Has(bad, "\r"));
  CHECK(!Has(bad, "\x01"));
  CHECK(Has(bad, "?"));
}

void TestPackageAndRequest() {
  TempFolder temp;
  const fs::path file = WriteLog(temp.path, LogName(1), kGameplaySession + "line\n", 0);
  const GameLog log{file, LogName(1), std::time(nullptr), fs::file_size(file), LogActivity::kGameplay, LogRole::kLastGameplay};
  const std::optional<IncidentPackage> package = BuildIncidentPackage("  The game crashes  ", log, SampleFacts(), Facts());
  CHECK(package.has_value());
  if (!package) return;
  CHECK(package->Value("description") == "The game crashes");
  CHECK(package->Value("launcher_version") == "0.1.0");
  CHECK(package->Value("guest") == "TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)");
  CHECK(package->Value("platform") == "Windows");
  CHECK(package->Value("operating_system") == "Windows 11 (build 26200)");
  CHECK(package->Value("runtime") == "ReXGlue SDK 0.10.0");
  CHECK(package->Value("log_file") == LogName(1));
  CHECK(package->Value("log_clipped") == "no");
  CHECK(Has(package->Value("log_text"), "Runtime configuration:"));
  const std::string preview = IncidentPreviewText(*package);
  CHECK(Has(preview, "Description:"));
  CHECK(Has(preview, "Log after privacy filtering:"));
  const std::string fields = IncidentRequestFields(*package);
  CHECK(fields.starts_with(",\"description\":\"The game crashes\""));
  CHECK(Has(fields, "\"guest\":\"TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)\""));
  CHECK(Has(fields, "\"runtime\":\"ReXGlue SDK 0.10.0\""));
  const std::string request = IncidentFileMessage(1, *package);
  CHECK(request.starts_with("{\"op\":\"incident.file\",\"seq\":1,"));
  const std::optional<trg::update::json::Value> parsed = trg::update::json::Parse(request);
  CHECK(parsed.has_value());
  if (parsed && parsed->IsObject()) {
    const auto& object = parsed->AsObject();
    CHECK(object.size() == 11);
    CHECK(parsed->Find("op") && parsed->Find("op")->AsString() == "incident.file");
    CHECK(parsed->Find("seq") && parsed->Find("seq")->IsNumber());
    const char* keys[] = {"description", "launcher_version", "guest", "platform", "operating_system",
                          "runtime", "log_file", "log_clipped", "log_text"};
    for (const char* key : keys) CHECK(object.contains(key));
    const trg::update::json::Value* description = parsed->Find("description");
    CHECK(description && description->IsString() && description->AsString() == "The game crashes");
    CHECK(object.find("game_build") == object.end());
  }
  CHECK(Has(request, "\\n"));
  CHECK(!Has(request, "log_clipped\":\"yes"));

  const std::optional<IncidentPackage> short_description = BuildIncidentPackage("  short  ", log, SampleFacts(), Facts());
  CHECK(!short_description.has_value());
  CHECK(IsIncidentReceipt(std::string(64, 'a')));
  CHECK(!IsIncidentReceipt(std::string(63, 'a')));
  CHECK(!IsIncidentReceipt(std::string(64, 'A')));
  CHECK(IncidentOutcomeOf("ok") == IncidentOutcome::kSent);
  CHECK(IncidentOutcomeOf("incident_hourly_limit") == IncidentOutcome::kRecentLimit);
  CHECK(IncidentOutcomeOf("incident_daily_limit") == IncidentOutcome::kDailyLimit);
  CHECK(IncidentOutcomeOf("too_large") == IncidentOutcome::kRefused);
  CHECK(IncidentOutcomeOf("timeout") == IncidentOutcome::kUnreachable);
  CHECK(IncidentOutcomeOf("weird") == IncidentOutcome::kServiceError);
}

// The same payload on Linux: the platform is Linux, the distribution is in the OS text, and Linux home and install
// paths are scrubbed like Windows ones.
void TestLinuxPackageAndScrubber() {
  const ReportFacts facts{"0.1.0", "TMNT: Turtles in Time Re-Shelled (XBLA) 0.0.1.3 (584109D2)", "Linux",
                          "Linux: Ubuntu 24.04 LTS", "ReXGlue SDK 0.10.0"};
  const std::string log_text =
      "Runtime configuration:\nRuntime OS: Linux: Ubuntu 24.04 LTS\n"
      "User data: /home/zefa/.local/share/TMNT\n"
      "Install: /home/zefa/Applications/TMNT/game/user_data/settings.toml\n"
      "peer 1234567890 connected 203.0.113.7\n";
  TempFolder temp;
  const fs::path file = WriteLog(temp.path, LogName(1), log_text, 0);
  const GameLog log{file, LogName(1), std::time(nullptr), fs::file_size(file), LogActivity::kGameplay, LogRole::kLastGameplay};
  PrivateFacts private_facts;
  private_facts.user_names = {"zefa"};
  private_facts.machine_names = {"turtle-deck"};
  private_facts.profile_folders = {"/home/zefa"};
  private_facts.install_folders = {"/home/zefa/Applications/TMNT"};
  const std::optional<IncidentPackage> package =
      BuildIncidentPackage("The game crashes on Linux", log, facts, private_facts);
  CHECK(package.has_value());
  if (!package) return;
  CHECK(package->Value("platform") == "Linux");
  CHECK(package->Value("operating_system") == "Linux: Ubuntu 24.04 LTS");
  CHECK(Has(package->Value("log_text"), "<home>"));
  CHECK(Has(package->Value("log_text"), "<install>"));
  CHECK(!Has(package->Value("log_text"), "/home/zefa"));
  CHECK(Has(package->Value("log_text"), "<ipv4>"));
  const std::string request = IncidentFileMessage(1, *package);
  CHECK(Has(request, "\"platform\":\"Linux\""));
  CHECK(Has(request, "\"operating_system\":\"Linux: Ubuntu 24.04 LTS\""));
}

void TestLinuxScrubber() {
  PrivateFacts facts;
  facts.user_names = {"zefa"};
  facts.machine_names = {"turtle-deck"};
  facts.profile_folders = {"/home/zefa", "/var/home/zefa", "/run/user/1000", "/mnt/games/zefa"};
  facts.install_folders = {"/home/zefa/Applications/TMNT"};
  const std::string text =
      "User data: /home/zefa/.local/share/tmnt_turtles_in_time_reshelled\n"
      "Config: /home/zefa/.config/tmnt/settings.toml\n"
      "Cache: /var/home/zefa/.cache/tmnt/shaders\n"
      "Runtime: /run/user/1000/tmnt.sock\n"
      "Custom data: /mnt/games/zefa/state\n"
      "AppImage: /home/zefa/Applications/TMNT/TurtlesInTimeReshelled-v0.1.0-x86_64.AppImage\n"
      "Mount: /tmp/.mount_TMNTabc123/usr/bin/tmnt_turtles_in_time_reshelled\n"
      "user zefa host turtle-deck\n"
      "driver Mesa 24.0.9\n";
  const std::string out = ScrubText(text, facts);
  CHECK(Has(out, "<home>/.local/share/tmnt_turtles_in_time_reshelled"));
  CHECK(Has(out, "<home>/.config/tmnt/settings.toml"));
  CHECK(Has(out, "<home>/.cache/tmnt/shaders"));
  CHECK(Has(out, "<home>/tmnt.sock"));
  CHECK(Has(out, "<home>/state"));
  CHECK(Has(out, "<install>/TurtlesInTimeReshelled-v0.1.0-x86_64.AppImage"));
  CHECK(!Has(out, "/home/zefa"));
  CHECK(!Has(out, "/var/home/zefa"));
  CHECK(!Has(out, "/run/user/1000"));
  CHECK(!Has(out, "/mnt/games/zefa"));
  CHECK(Has(out, "/tmp/.mount_TMNTabc123/usr/bin/tmnt_turtles_in_time_reshelled"));
  CHECK(Has(out, "<user>"));
  CHECK(Has(out, "<computer>"));
  CHECK(!Has(out, "turtle-deck"));
  CHECK(Has(out, "Mesa 24.0.9"));
}

void TestWireVectors() {
  const fs::path path = fs::path(TMNT_SOURCE_DIR) / "tests" / "incident_wire_vectors.txt";
  const std::vector<std::string> messages = WireMessages();
  CHECK(messages.size() == 3);
  if (messages.size() != 3) return;
  if (std::getenv("TMNT_WRITE_INCIDENT_VECTORS")) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const std::string& message : messages) out << message << '\n';
  }
  CHECK(ReadMessages(path) == messages);
  const char* operations[] = {"incident.file", "incident.status", "incident.read"};
  for (size_t i = 0; i < messages.size(); ++i) {
    const std::optional<trg::update::json::Value> parsed = trg::update::json::Parse(messages[i]);
    CHECK(parsed.has_value());
    if (!parsed || !parsed->IsObject()) continue;
    const trg::update::json::Value* op = parsed->Find("op");
    CHECK(op && op->IsString() && op->AsString() == operations[i]);
    CHECK(parsed->Find("seq") && parsed->Find("seq")->IsNumber());
  }
  if (const std::optional<trg::update::json::Value> file = trg::update::json::Parse(messages[0]);
      file && file->IsObject()) {
    CHECK(file->AsObject().size() == 11);
  }
  if (const std::optional<trg::update::json::Value> status = trg::update::json::Parse(messages[1]);
      status && status->IsObject()) {
    CHECK(status->AsObject().size() == 3);
    const trg::update::json::Value* reports = status->Find("reports");
    CHECK(reports && reports->IsArray() && reports->AsArray().size() == 1);
  }
  if (const std::optional<trg::update::json::Value> read = trg::update::json::Parse(messages[2]);
      read && read->IsObject()) {
    CHECK(read->AsObject().size() == 4);
    CHECK(read->Find("ticket") && read->Find("ticket")->AsString() == "TMNT-ABCDEF");
    CHECK(read->Find("receipt") && read->Find("receipt")->AsString() == std::string(64, 'a'));
  }
}

void TestClipping() {
  TempFolder temp;
  std::string text = kGameplaySession;
  while (text.size() < 300 * 1024) text += "a long diagnostic line with 0123456789\n";
  const fs::path file = WriteLog(temp.path, LogName(9), text, 0);
  const GameLog log{file, LogName(9), std::time(nullptr), fs::file_size(file), LogActivity::kGameplay, LogRole::kLastGameplay};
  const std::optional<IncidentPackage> package = BuildIncidentPackage("Large log to shorten", log, SampleFacts(), Facts());
  CHECK(package.has_value());
  if (!package) return;
  CHECK(package->Value("log_clipped") == "yes");
  CHECK(package->Value("log_text").size() <= kMostLogBytes);
  CHECK(package->Value("log_text").starts_with("a long diagnostic line"));
}

void TestStatusParsing() {
  const std::string answer =
      "{\"reports\":[{\"ticket\":\"TMNT-ABCDEF\",\"status\":\"fixed\",\"status_updated_at\":123,"
      "\"fixed_version\":\"0.2.0\",\"player_message\":\"Fixed in the next build.\"}]}";
  const std::vector<ReportStatus> statuses = ReadReportStatuses(answer);
  CHECK(statuses.size() == 1);
  CHECK(statuses[0].ticket == "TMNT-ABCDEF");
  CHECK(statuses[0].status == "fixed");
  CHECK(statuses[0].updated_at == 123);
  CHECK(statuses[0].fixed_version == "0.2.0");
  CHECK(statuses[0].player_message == "Fixed in the next build.");
  CHECK(ReadReportStatuses("{}").empty());
  // A malformed answer must leave the last known state alone, not crash or invent a status.
  CHECK(ReadReportStatuses("not json").empty());
  CHECK(ReadReportStatuses("[]").empty());
  CHECK(ReadReportStatuses("{\"reports\":[1,2]}").empty());
  CHECK(SentReportOf("not json").fields.empty());
  CHECK(SentReportOf("{\"report\":[]}").fields.empty());
  TrackedReport tracked;
  tracked.ticket = "TMNT-ABCDEF";
  tracked.receipt = std::string(64, 'a');
  tracked.submitted_at = 1;
  const std::string status_envelope = IncidentStatusMessage(1, {tracked});
  CHECK(Has(status_envelope, "\"reports\":["));
  CHECK(Has(status_envelope, "\"receipt\":\"" + std::string(64, 'a') + "\""));
  const std::optional<trg::update::json::Value> status_parsed = trg::update::json::Parse(status_envelope);
  CHECK(status_parsed.has_value());
  if (status_parsed && status_parsed->IsObject()) {
    CHECK(status_parsed->AsObject().size() == 3);
    const trg::update::json::Value* op = status_parsed->Find("op");
    CHECK(op && op->AsString() == "incident.status");
    const trg::update::json::Value* reports = status_parsed->Find("reports");
    CHECK(reports && reports->IsArray() && reports->AsArray().size() == 1);
  }
  const std::string read_envelope = IncidentReadMessage(1, tracked);
  const std::optional<trg::update::json::Value> read_parsed = trg::update::json::Parse(read_envelope);
  CHECK(read_parsed.has_value());
  if (read_parsed && read_parsed->IsObject()) {
    CHECK(read_parsed->AsObject().size() == 4);
    const trg::update::json::Value* op = read_parsed->Find("op");
    CHECK(op && op->AsString() == "incident.read");
    const trg::update::json::Value* ticket = read_parsed->Find("ticket");
    CHECK(ticket && ticket->IsString() && ticket->AsString() == "TMNT-ABCDEF");
    const trg::update::json::Value* receipt = read_parsed->Find("receipt");
    CHECK(receipt && receipt->AsString() == std::string(64, 'a'));
  }
}

TrackedReport SampleReport(const std::string& ticket, int64_t submitted, const std::string& status, const std::string& message) {
  TrackedReport report;
  report.ticket = ticket;
  report.receipt = std::string(64, 'a');
  report.submitted_at = submitted;
  report.status_updated_at = submitted;
  report.description = "Description";
  report.status = status;
  report.player_message = message;
  return report;
}

void TestHistory() {
  TempFolder temp;
  const fs::path file = temp.path / kReportHistoryFile;
  ReportHistory history(file);
  CHECK(history.Add(SampleReport("TMNT-AAAAAA", 10, "new", "")));
  CHECK(history.Add(SampleReport("TMNT-BBBBBB", 20, "new", "")));
  CHECK(history.reports().size() == 2);
  ReportHistory loaded(file);
  loaded.Load();
  CHECK(loaded.reports().size() == 2);
  CHECK(loaded.reports()[0].ticket == "TMNT-BBBBBB");
  CHECK(loaded.Apply({ReportStatus{"TMNT-AAAAAA", "investigating", 30, "", ""}}, {"TMNT-AAAAAA", "TMNT-BBBBBB"}));
  CHECK(loaded.reports()[0].unavailable);
  CHECK(loaded.reports()[1].unseen);
  CHECK(loaded.reports()[1].ticket == "TMNT-AAAAAA");
  CHECK(loaded.HasUnseen());
  CHECK(!loaded.HasUnseenMessage());
  CHECK(loaded.NextUnseen() == "TMNT-AAAAAA");
  CHECK(loaded.MarkSeen("TMNT-AAAAAA"));
  CHECK(!loaded.HasUnseen());

  CHECK(loaded.Apply({ReportStatus{"TMNT-AAAAAA", "fixed", 40, "0.2.0", "Fixed"}}, {"TMNT-AAAAAA"}));
  CHECK(loaded.reports()[1].message_unseen);
  CHECK(loaded.HasUnseenMessage());
  CHECK(loaded.NextUnseen() == "TMNT-AAAAAA");
  CHECK(loaded.Save());

  ReportHistory damaged(file);
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "not json";
  }
  damaged.Load();
  CHECK(damaged.reports().empty());
  CHECK(fs::exists(file.string() + ".damaged"));
}

void TestDeletedHistoryEntry() {
  TempFolder temp;
  ReportHistory history(temp.path / kReportHistoryFile);
  CHECK(history.Add(SampleReport("TMNT-CCCCCC", 10, "new", "")));
  CHECK(history.Apply({}, {"TMNT-CCCCCC"}));
  CHECK(history.reports()[0].unavailable);
  CHECK(!history.Apply({}, {"TMNT-CCCCCC"}));
  CHECK(history.Remove("TMNT-CCCCCC"));
  CHECK(history.reports().empty());
}

void TestFailedSubmissionIsNotRemembered() {
  const std::string page = ReadSource("src/incident_page.cpp");
  CHECK(Has(page, "if (result_->outcome == IncidentOutcome::kSent) sent_report_remembered_ = my_reports_.Remember"));
  const std::string reports = ReadSource("src/my_reports.cpp");
  CHECK(Has(reports, "if (receipt.credential.empty()) return false;"));
}

}  // namespace

int main() {
  TestNewestFiveFirst();
  TestRotationAndEmptyLogs();
  TestSessionsThatAreNotGameLogs();
  TestCurrentSessionAndLastGameplay();
  TestDefaultGameLog();
  TestCharacterBounds();
  TestLastBytes();
  TestScrubber();
  TestPackageAndRequest();
  TestLinuxPackageAndScrubber();
  TestLinuxScrubber();
  TestWireVectors();
  TestClipping();
  TestStatusParsing();
  TestHistory();
  TestDeletedHistoryEntry();
  TestFailedSubmissionIsNotRemembered();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
