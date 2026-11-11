// The Bug Report page's work without a window: the logs it offers, the report it builds and the sending of it.
//
// A report is built into an IncidentPackage, whether or not the player previews it. The preview and the request both read
// that package's fields, so what the player can read is what goes out.
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log_scrubber.h"
#include "report_history.h"

namespace tmnt {

inline constexpr size_t kMostGameLogs = 5;
inline constexpr size_t kFewestDescriptionCharacters = 10;
inline constexpr size_t kMostDescriptionCharacters = 500;
// Twice the largest log of a long support session. A log with 64 KiB of line breaks still fits the service's frame once
// JSON has escaped them (server/tmnt-lobby/src/incidents.js).
inline constexpr size_t kMostLogBytes = 64 * 1024;

// How far a session got: kGameStarted left the launcher for the game, kGameplay reached the game's own code.
enum class LogActivity { kLauncher, kGameStarted, kGameplay };

// kNone is a session that started the game without reaching its own code.
enum class LogRole { kNone, kCurrentLauncher, kCurrentGameplay, kLastGameplay, kOtherGameplay, kOtherLauncher };

struct GameLog {
  std::filesystem::path path;
  std::string name;
  std::time_t written = 0;
  uintmax_t bytes = 0;
  LogActivity activity = LogActivity::kLauncher;
  LogRole role = LogRole::kNone;
};

// The logs in `folder`: `running_log` (this launcher's own session) first, then newest first. Left out: other files and
// sessions that closed in the launcher without starting the game.
std::vector<GameLog> FindGameLogs(const std::filesystem::path& folder, const std::filesystem::path& running_log,
                                  size_t most = kMostGameLogs);

// The log a report is most likely about: the newest that started the game, not this launcher's own session.
size_t DefaultGameLog(const std::vector<GameLog>& logs);

// English, for Tr; nullptr for kNone.
const char* LogRoleText(LogRole role);
const char* PreviewButtonText(bool preview_open);

// "Nov 5, 2026 03:51", or "05/11/2026 03:51" in Portuguese; local time.
std::string LogDateText(std::time_t written, bool portuguese);
// "184 KB", "1.2 MB".
std::string LogSizeText(uintmax_t bytes);

// The bytes of `text` that hold its first `most` UTF-8 characters.
size_t BytesOfCharacters(std::string_view text, size_t most);
size_t CountCharacters(std::string_view text);

enum class DraftProblem { kNone, kDescriptionTooShort, kNoLog };
// Spaces around the description do not count toward its length.
DraftProblem CheckDraft(std::string_view description, bool has_log);

// The fields of the installed game and this PC that go with every report.
struct ReportFacts {
  std::string launcher_version;
  std::string guest;
  std::string platform;
  std::string operating_system;
  std::string runtime;
};

// "Windows 11 (build 26200)", "Ubuntu 24.04 LTS".
std::string OperatingSystemText();

struct IncidentField {
  std::string_view key;    // the request's member
  std::string_view label;  // English; the preview translates it
  std::string value;
};

struct IncidentPackage {
  std::vector<IncidentField> fields;
  const std::string& Value(std::string_view key) const;
};

// The end of `text`, at most `most` bytes, starting on a whole line.
std::string LastBytes(std::string_view text, size_t most);

// Scrubs the log's text and bounds it. nullopt when the log cannot be read or holds nothing, or when the scrubbed
// description is shorter than the minimum.
std::optional<IncidentPackage> BuildIncidentPackage(std::string_view description, const GameLog& log, const ReportFacts& facts,
                                                    const PrivateFacts& private_facts);

std::string IncidentPreviewText(const IncidentPackage& package);
// The `incident.file` members, with the comma that joins them to the request's op and seq.
std::string IncidentRequestFields(const IncidentPackage& package);
// The exact WebSocket messages used in production; tests parse these same strings.
std::string IncidentFileMessage(int seq, const IncidentPackage& package);
std::string IncidentStatusMessage(int seq, const std::vector<TrackedReport>& reports);
std::string IncidentReadMessage(int seq, const TrackedReport& report);

enum class IncidentOutcome { kSent, kRecentLimit, kDailyLimit, kUnreachable, kRefused, kNotSupported, kServiceError };

// What the service's answer means for the page.
IncidentOutcome IncidentOutcomeOf(const std::string& code);

// English, for Tr; nullptr unless `outcome` is one of the two limits.
const char* IncidentLimitTitle(IncidentOutcome outcome);

struct IncidentReceipt {
  IncidentOutcome outcome = IncidentOutcome::kServiceError;
  std::string ticket;
  // Proves ownership of this one report. Never logged.
  std::string credential;
  int64_t submitted_at = 0;
};

bool IsIncidentTicket(std::string_view text);
bool IsIncidentReceipt(std::string_view text);

// What My reports keeps of a sent report.
TrackedReport TrackedReportOf(const IncidentPackage& package, const IncidentReceipt& receipt);

// The newest reports in `reports` as the `incident.status` members, with the comma that joins them to op and seq.
std::string IncidentStatusRequest(const std::vector<TrackedReport>& reports);
// The `incident.read` members, with the comma that joins them to op and seq.
std::string IncidentReadRequest(const TrackedReport& report);
std::vector<ReportStatus> ReadReportStatuses(std::string_view answer);
// The stored report as the same fields a preview shows.
IncidentPackage SentReportOf(std::string_view report_json);

inline constexpr char kIncidentOperation[] = "incident.file";
inline constexpr char kIncidentStatusOperation[] = "incident.status";
inline constexpr char kIncidentReadOperation[] = "incident.read";

struct IncidentServiceConfig {
  std::string url;
  std::function<void(const std::string&)> log;  // never receives a receipt, token or report text
};

// The current online service address, including the developer TMNT_LOBBY override.
IncidentServiceConfig IncidentServiceFromEnvironment();

// Signs in without showing the player online, sends the package and closes. Blocks: run it off the UI thread. Online Play
// does not have to be on, and the online identity is made if this PC has none.
IncidentReceipt FileIncident(const IncidentServiceConfig& config, const std::filesystem::path& user_dir,
                             const IncidentPackage& package);

// Both block, take no account and need no Online Play: the receipts are the only proof sent. nullopt when the service could
// not answer, or for FetchSentReport when it no longer has the report. An answer to FetchReportStatuses leaves out each
// report whose receipt the service does not know, which is how a report deleted there shows.
std::optional<std::vector<ReportStatus>> FetchReportStatuses(const IncidentServiceConfig& config,
                                                             const std::vector<TrackedReport>& reports);
std::optional<IncidentPackage> FetchSentReport(const IncidentServiceConfig& config, const TrackedReport& report);

}  // namespace tmnt
