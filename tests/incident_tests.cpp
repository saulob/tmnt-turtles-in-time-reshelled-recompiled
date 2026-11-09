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

ReportFacts SampleFacts() { return {"0.1.0", "TMNT: Turtles in Time Re-Shelled (XBLA)", "0.0.1.3", "584109D2", "Windows 11 (build 26200)"}; }

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
  CHECK(package->Value("game_id") == "584109D2");
  CHECK(package->Value("log_file") == LogName(1));
  CHECK(package->Value("log_clipped") == "no");
  CHECK(Has(package->Value("log_text"), "Runtime configuration:"));
  const std::string preview = IncidentPreviewText(*package);
  CHECK(Has(preview, "Description:"));
  CHECK(Has(preview, "Log after privacy filtering:"));
  const std::string request = IncidentRequestBody(*package);
  CHECK(request.starts_with("{"));
  CHECK(Has(request, "\"description\":\"The game crashes\""));
  CHECK(Has(request, "\\n"));
  CHECK(!Has(request, "log_clipped\":\"yes"));

  const std::optional<IncidentPackage> missing = BuildIncidentPackage("text", log, SampleFacts(), Facts());
  CHECK(missing.has_value());
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

void TestClipping() {
  TempFolder temp;
  std::string text = kGameplaySession;
  while (text.size() < 300 * 1024) text += "a long diagnostic line with 0123456789\n";
  const fs::path file = WriteLog(temp.path, LogName(9), text, 0);
  const GameLog log{file, LogName(9), std::time(nullptr), fs::file_size(file), LogActivity::kGameplay, LogRole::kLastGameplay};
  const std::optional<IncidentPackage> package = BuildIncidentPackage("Large log", log, SampleFacts(), Facts());
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
  TrackedReport tracked;
  tracked.ticket = "TMNT-ABCDEF";
  tracked.receipt = std::string(64, 'a');
  tracked.submitted_at = 1;
  const std::string request = IncidentStatusRequest({tracked});
  CHECK(Has(request, "\"reports\":["));
  CHECK(Has(request, "\"receipt\":\"" + std::string(64, 'a') + "\""));
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
  TestClipping();
  TestStatusParsing();
  TestHistory();
  TestDeletedHistoryEntry();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
