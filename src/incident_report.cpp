#include "incident_report.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <system_error>

#include "incident_json.h"
#include "localization/ui_text.h"
#include "online/identity.h"
#include "online/online_service.h"
#include "online/protocol.h"
#include "online/websocket.h"
#include "online_setting.h"
#include "port_version.h"
#include "updater/json.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tmnt {
namespace fs = std::filesystem;
namespace json = trg::update::json;
namespace {

constexpr std::string_view kLogPrefix = "tmnt_turtles_in_time_reshelled_";
constexpr std::string_view kLogSuffix = ".log";
constexpr size_t kFewestLogDigits = 3, kMostLogDigits = 10;
constexpr size_t kLogsLookedAt = 200;
constexpr size_t kSessionHeadBytes = 128 * 1024;
// More than the report keeps, so that scrubbing cannot leave it short.
constexpr size_t kLogTailBytes = 256 * 1024;

// The launcher and the runtime write these.
constexpr std::string_view kGameStartMarks[] = {"Launcher closed, starting the game", "Restarted in a new window"};
constexpr std::string_view kRuntimeMarks[] = {"Runtime configuration:", "Guest memory arena mapped:", "Execution complete:"};
constexpr std::string_view kLauncherClosedMark = "Launcher closed";

constexpr char kDescriptionKey[] = "description";
constexpr char kLogTextKey[] = "log_text";
constexpr size_t kReceiptHexDigits = 64;
constexpr size_t kMostMetadataCharacters = 64;

struct FieldName {
  std::string_view key;
  std::string_view label;
};
constexpr FieldName kFieldNames[] = {
    {kDescriptionKey, "Description"},
    {"launcher_version", "Launcher version"},
    {"guest", "Game image"},
    {"platform", "Platform"},
    {"operating_system", "Operating system"},
    {"runtime", "Runtime"},
    {"log_file", "Selected log"},
    {"log_clipped", "Log shortened to its end"},
    {kLogTextKey, "Log after privacy filtering"},
};

IncidentReceipt NotSent(IncidentOutcome outcome) {
  IncidentReceipt receipt;
  receipt.outcome = outcome;
  return receipt;
}

void Say(const IncidentServiceConfig& config, const std::string& line) {
  if (config.log) config.log(line);
}

std::string EndpointHost(std::string_view url) {
  const size_t scheme = url.find("://");
  const std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
  const size_t end = rest.find_first_of(":/?#");
  const std::string host(rest.substr(0, end));
  return host.empty() ? "unknown" : host;
}

std::string RequestKeys(const IncidentPackage& package) {
  std::string keys;
  for (const IncidentField& field : package.fields) {
    if (!keys.empty()) keys += ',';
    keys += field.key;
  }
  return keys;
}

std::string WorkerBuild(const json::Value* capabilities) {
  const json::Value* build = capabilities ? capabilities->Find("worker_build") : nullptr;
  return build && build->IsString() ? build->AsString() : std::string("unknown");
}

std::string StringOf(const json::Value* value) { return value && value->IsString() ? value->AsString() : std::string(); }
bool BoolOf(const json::Value* value, bool fallback) { return value && value->IsBool() ? value->AsBool() : fallback; }
int64_t Int64Of(const json::Value* value) {
  const auto number = value && value->IsNumber() ? value->AsInt64() : std::nullopt;
  return number ? *number : 0;
}

constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool IsDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool IsGameLogName(std::string_view name) {
  if (!name.starts_with(kLogPrefix) || !name.ends_with(kLogSuffix)) return false;
  name.remove_prefix(kLogPrefix.size());
  name.remove_suffix(kLogSuffix.size());
  // A rotating sink may leave `<name>.1.log`, `<name>.2.log` beside the current file.
  if (const size_t dot = name.rfind('.'); dot != std::string_view::npos) {
    const std::string_view rotation = name.substr(dot + 1);
    if (!IsDigits(rotation) || rotation.size() > 3) return false;
    name = name.substr(0, dot);
  }
  return name.size() >= kFewestLogDigits && name.size() <= kMostLogDigits && IsDigits(name);
}

std::string ReadPart(const fs::path& file, uintmax_t offset, size_t size) {
  std::ifstream in(file, std::ios::binary);
  std::string text(size, '\0');
  in.seekg(std::streamoff(offset));
  in.read(text.data(), std::streamsize(size));
  text.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
  return text;
}

struct Session {
  LogActivity activity = LogActivity::kLauncher;
  bool closed_in_launcher = false;
};

Session ReadSession(const fs::path& file) {
  const std::string head = ReadPart(file, 0, kSessionHeadBytes);
  const auto holds = [&](std::string_view mark) { return head.find(mark) != std::string::npos; };
  if (std::any_of(std::begin(kRuntimeMarks), std::end(kRuntimeMarks), holds)) return {LogActivity::kGameplay, false};
  if (std::any_of(std::begin(kGameStartMarks), std::end(kGameStartMarks), holds)) return {LogActivity::kGameStarted, false};
  return {LogActivity::kLauncher, holds(kLauncherClosedMark)};
}

std::time_t SystemTimeOf(fs::file_time_type written) {
  const auto system = std::chrono::system_clock::now() +
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now());
  return std::chrono::system_clock::to_time_t(system);
}

struct Candidate {
  fs::path path;
  std::string name;
  fs::file_time_type written;
  uintmax_t bytes = 0;
  bool running = false;
};

bool IsRunningLog(const fs::path& file, const fs::path& running_log) {
  std::error_code ec;
  return !running_log.empty() && (file == running_log || fs::equivalent(file, running_log, ec));
}

// A directory listing lags behind a file that is still open, so the running log is asked for its own size and time.
std::vector<Candidate> LogCandidates(const fs::path& folder, const fs::path& running_log) {
  std::vector<Candidate> found;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry;
    const std::string name = it->path().filename().string();
    if (!IsGameLogName(name) || !it->is_regular_file(entry) || it->is_symlink(entry)) continue;
    const bool running = IsRunningLog(it->path(), running_log);
    const uintmax_t bytes = running ? fs::file_size(it->path(), entry) : it->file_size(entry);
    const fs::file_time_type written = running ? fs::last_write_time(it->path(), entry) : it->last_write_time(entry);
    if (entry || (bytes == 0 && !running)) continue;
    found.push_back({it->path(), name, written, bytes, running});
  }
  std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
    if (a.running != b.running) return a.running;
    return a.written != b.written ? a.written > b.written : a.name > b.name;
  });
  return found;
}

void MarkLastGameplay(std::vector<GameLog>& logs) {
  const auto played = [](const GameLog& log) { return log.activity == LogActivity::kGameplay; };
  const auto last = std::find_if(logs.begin(), logs.end(), played);
  if (last != logs.end() && last->role == LogRole::kNone) last->role = LogRole::kLastGameplay;
  for (GameLog& log : logs) {
    if (log.role != LogRole::kNone) continue;
    if (log.activity == LogActivity::kGameplay) log.role = LogRole::kOtherGameplay;
    else if (log.activity == LogActivity::kLauncher) log.role = LogRole::kOtherLauncher;
  }
}

std::string TrimmedDescription(std::string_view text) {
  constexpr std::string_view kSpace = " \t\r\n";
  const size_t first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return {};
  return std::string(text.substr(first, text.find_last_not_of(kSpace) - first + 1));
}

// A metadata field the service accepts: no more than its limit, no spaces at either end.
std::string Clamped(std::string text) {
  text.resize(BytesOfCharacters(text, kMostMetadataCharacters));
  return TrimmedDescription(text);
}

#ifdef _WIN32
std::string WindowsVersionText() {
  // GetVersionEx answers with the version the manifest declares, not the running one.
  using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get_version = ntdll ? reinterpret_cast<GetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  if (!get_version || get_version(&version) != 0) return "Windows";
  constexpr DWORD kFirstWindows11Build = 22000;
  const char* name = version.dwMajorVersion == 10 ? (version.dwBuildNumber >= kFirstWindows11Build ? "Windows 11" : "Windows 10")
                                                  : "Windows";
  return std::string(name) + " (build " + std::to_string(version.dwBuildNumber) + ")";
}
#else
std::string LinuxDistributionText() {
  std::ifstream in("/etc/os-release");
  constexpr std::string_view kKey = "PRETTY_NAME=";
  for (std::string line; std::getline(in, line);) {
    if (!line.starts_with(kKey)) continue;
    std::string value = line.substr(kKey.size());
    std::erase(value, '"');
    return value;
  }
  return {};
}
#endif

std::string Envelope(std::string_view op, int seq, std::string_view fields = {}) {
  return "{\"op\":" + JsonQuote(op) + ",\"seq\":" + std::to_string(seq) + std::string(fields) + "}";
}

class Watchdog {
 public:
  Watchdog(std::shared_ptr<online::WebSocket> socket, int timeout_ms)
      : socket_(std::move(socket)), thread_([this, timeout_ms] {
          std::unique_lock<std::mutex> lock(mutex_);
          if (!wake_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return done_; })) socket_->close();
        }) {}

  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::shared_ptr<online::WebSocket> socket_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_ = false;
  std::thread thread_;
};

class ServiceSession {
 public:
  bool Connect(const std::string& url, std::string* error) {
    socket_ = std::shared_ptr<online::WebSocket>(online::WebSocket::connect(url, 8000, error));
    return socket_ != nullptr;
  }

  std::optional<json::Value> Call(std::string_view op, std::string_view fields, std::string* code) {
    const int seq = TakeSeq();
    return CallRaw(Envelope(op, seq, fields), seq, code);
  }

  std::optional<json::Value> CallRaw(const std::string& message, int seq, std::string* code) {
    if (!socket_) {
      if (code) *code = "unreachable";
      return std::nullopt;
    }
    if (!socket_->send_text(message)) {
      if (code) *code = "unreachable";
      return std::nullopt;
    }
    Watchdog watchdog(socket_, 20000);
    for (int frames = 0; frames < 200; ++frames) {
      std::string text;
      bool binary = false;
      if (!socket_->receive(text, binary) || binary) {
        if (code) *code = "timeout";
        return std::nullopt;
      }
      const std::optional<json::Value> root = json::Parse(text);
      if (!root || !root->IsObject()) continue;
      const json::Value* re = root->Find("re");
      if (!re || !re->IsNumber() || re->AsInt64().value_or(-1) != seq) continue;  // an event sent while waiting
      if (code) *code = StringOf(root->Find("code"));
      return root;
    }
    if (code) *code = "timeout";
    return std::nullopt;
  }

  int TakeSeq() { return next_seq_++; }

  void Close() {
    if (socket_) socket_->close();
    socket_.reset();
  }

  ~ServiceSession() { Close(); }

 private:
  std::shared_ptr<online::WebSocket> socket_;
  int next_seq_ = 1;
};

std::string HelloBody(const online::Identity& identity) {
  std::string fields = ",\"proto\":" + std::to_string(online::kLobbyProtocol);
  fields += ",\"game\":" + JsonQuote(online::kGameId);
  fields += ",\"player\":" + JsonQuote(identity.player_id);
  fields += ",\"token\":" + JsonQuote(identity.token);
  fields += ",\"guest\":" + JsonQuote(online::kGuestBase);
  fields += ",\"build\":" + std::to_string(online::kOnlineBuild);
  fields += ",\"name\":\"Player\"";
  fields += ",\"version\":" + JsonQuote(PortVersion());
#if defined(_WIN32)
  fields += ",\"platform\":\"Windows\"";
#else
  fields += ",\"platform\":\"Linux\"";
#endif
  fields += ",\"presence\":false,\"relay\":false";
  return fields;
}

std::string JoinedValues(const IncidentPackage& package) {
  std::string body;
  for (const IncidentField& field : package.fields) {
    if (!body.empty()) body += ',';
    body += JsonQuote(field.key);
    body += ':';
    body += JsonQuote(field.value);
  }
  return body;
}

std::vector<ReportStatus> StatusesOf(const json::Value& answer) {
  std::vector<ReportStatus> statuses;
  const json::Value* list = answer.Find("reports");
  if (!list || !list->IsArray()) return statuses;
  for (const json::Value& entry : list->AsArray()) {
    if (!entry.IsObject()) continue;
    statuses.push_back({StringOf(entry.Find("ticket")), StringOf(entry.Find("status")), Int64Of(entry.Find("status_updated_at")),
                        StringOf(entry.Find("fixed_version")), StringOf(entry.Find("player_message"))});
  }
  return statuses;
}

IncidentPackage ReportOf(const json::Value* report) {
  IncidentPackage package;
  if (!report || !report->IsObject()) return package;
  for (const FieldName& name : kFieldNames) package.fields.push_back({name.key, name.label, StringOf(report->Find(name.key))});
  return package;
}

}  // namespace

std::vector<GameLog> FindGameLogs(const fs::path& folder, const fs::path& running_log, size_t most) {
  std::vector<GameLog> logs;
  std::vector<Candidate> candidates = LogCandidates(folder, running_log);
  if (candidates.size() > kLogsLookedAt) candidates.resize(kLogsLookedAt);
  for (const Candidate& candidate : candidates) {
    if (logs.size() == most) break;
    const Session session = ReadSession(candidate.path);
    // A session that ended without closing the launcher stays: that is what a launcher crash leaves.
    if (session.closed_in_launcher && !candidate.running) continue;
    LogRole role = LogRole::kNone;
    if (candidate.running) {
      role = session.activity == LogActivity::kGameplay ? LogRole::kCurrentGameplay : LogRole::kCurrentLauncher;
    }
    logs.push_back({candidate.path, candidate.name, SystemTimeOf(candidate.written), candidate.bytes, session.activity, role});
  }
  MarkLastGameplay(logs);
  return logs;
}

size_t DefaultGameLog(const std::vector<GameLog>& logs) {
  const auto started = std::find_if(logs.begin(), logs.end(), [](const GameLog& log) { return log.activity != LogActivity::kLauncher; });
  if (started != logs.end()) return size_t(started - logs.begin());
  const auto earlier = std::find_if(logs.begin(), logs.end(), [](const GameLog& log) {
    return log.role != LogRole::kCurrentLauncher && log.role != LogRole::kCurrentGameplay;
  });
  return earlier == logs.end() ? 0 : size_t(earlier - logs.begin());
}

const char* LogRoleText(LogRole role) {
  switch (role) {
    case LogRole::kCurrentLauncher: return "Current launcher session";
    case LogRole::kCurrentGameplay: return "Current session \xC2\xB7 Launcher + gameplay";
    case LogRole::kLastGameplay: return "Last gameplay";
    case LogRole::kOtherGameplay: return "Gameplay session";
    case LogRole::kOtherLauncher: return "Launcher only";
    case LogRole::kNone: break;
  }
  return nullptr;
}

const char* PreviewButtonText(bool preview_open) { return preview_open ? "Close preview" : "Open preview"; }

std::string LogDateText(std::time_t written, bool portuguese) {
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &written);
#else
  localtime_r(&written, &local);
#endif
  char text[40];
  if (portuguese) {
    std::snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
                  local.tm_hour, local.tm_min);
  } else {
    std::snprintf(text, sizeof(text), "%s %d, %04d %02d:%02d", kMonths[std::clamp(local.tm_mon, 0, 11)], local.tm_mday,
                  local.tm_year + 1900, local.tm_hour, local.tm_min);
  }
  return text;
}

std::string LogSizeText(uintmax_t bytes) {
  constexpr uintmax_t kKilobyte = 1024, kMegabyte = kKilobyte * 1024;
  char text[32];
  if (bytes < kMegabyte) {
    std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>(std::max<uintmax_t>(1, (bytes + kKilobyte / 2) / kKilobyte)));
  } else {
    std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / double(kMegabyte));
  }
  return text;
}

size_t BytesOfCharacters(std::string_view text, size_t most) {
  size_t characters = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (characters == most) return i;
    ++characters;
  }
  return text.size();
}

size_t CountCharacters(std::string_view text) {
  return size_t(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

DraftProblem CheckDraft(std::string_view description, bool has_log) {
  if (CountCharacters(TrimmedDescription(description)) < kFewestDescriptionCharacters) return DraftProblem::kDescriptionTooShort;
  return has_log ? DraftProblem::kNone : DraftProblem::kNoLog;
}

std::string OperatingSystemText() {
#ifdef _WIN32
  return WindowsVersionText();
#else
  const std::string distribution = LinuxDistributionText();
  return distribution.empty() ? "Linux" : "Linux: " + distribution;
#endif
}

const std::string& IncidentPackage::Value(std::string_view key) const {
  static const std::string missing;
  const auto found = std::find_if(fields.begin(), fields.end(), [&](const IncidentField& field) { return field.key == key; });
  return found == fields.end() ? missing : found->value;
}

std::string LastBytes(std::string_view text, size_t most) {
  if (text.size() <= most) return std::string(text);
  std::string_view tail = text.substr(text.size() - most);
  const size_t line = tail.find('\n');
  if (line != std::string_view::npos && line + 1 < tail.size()) tail.remove_prefix(line + 1);
  return std::string(tail);
}

std::optional<IncidentPackage> BuildIncidentPackage(std::string_view description, const GameLog& log, const ReportFacts& facts,
                                                    const PrivateFacts& private_facts) {
  std::error_code ec;
  const uintmax_t size = fs::file_size(log.path, ec);
  if (ec || size == 0) return std::nullopt;
  const uintmax_t offset = size > kLogTailBytes ? size - kLogTailBytes : 0;
  std::string raw = ReadPart(log.path, offset, size_t(size - offset));
  if (raw.empty()) return std::nullopt;
  if (offset > 0) {
    const size_t line = raw.find('\n');
    if (line != std::string::npos) raw.erase(0, line + 1);
  }
  const std::string scrubbed = ScrubText(raw, private_facts);
  std::string text = LastBytes(scrubbed, kMostLogBytes);
  if (text.find_first_not_of(" \t\n") == std::string::npos) return std::nullopt;
  const bool clipped = offset > 0 || text.size() < scrubbed.size();

  std::string summary = TrimmedDescription(ScrubText(TrimmedDescription(description), private_facts));
  summary.resize(BytesOfCharacters(summary, kMostDescriptionCharacters));
  summary = TrimmedDescription(summary);
  if (CountCharacters(summary) < kFewestDescriptionCharacters) return std::nullopt;

  std::string values[] = {std::move(summary),
                          Clamped(facts.launcher_version),
                          Clamped(facts.guest),
                          Clamped(facts.platform),
                          Clamped(facts.operating_system),
                          Clamped(facts.runtime),
                          log.name,
                          clipped ? "yes" : "no",
                          std::move(text)};
  IncidentPackage package;
  for (size_t i = 0; i < std::size(kFieldNames); ++i) {
    package.fields.push_back({kFieldNames[i].key, kFieldNames[i].label, std::move(values[i])});
  }
  return package;
}

IncidentPackage SentReportOf(std::string_view report_json) {
  const std::optional<json::Value> root = json::Parse(report_json);
  return ReportOf(root && root->IsObject() ? root->Find("report") : nullptr);
}

std::string IncidentPreviewText(const IncidentPackage& package) {
  std::string out;
  for (const IncidentField& field : package.fields) {
    const std::string label = Tr(std::string(field.label));
    if (field.key == kDescriptionKey || field.key == kLogTextKey) {
      out += (out.empty() ? "" : "\n") + label + ":\n" + field.value + "\n";
      if (field.key == kDescriptionKey) out += "\n";
    } else {
      out += label + ": " + (field.value.empty() ? "-" : field.value) + "\n";
    }
  }
  return out;
}

std::string IncidentRequestFields(const IncidentPackage& package) { return "," + JoinedValues(package); }

IncidentOutcome IncidentOutcomeOf(const std::string& code) {
  if (code == "ok") return IncidentOutcome::kSent;
  if (code == "incident_hourly_limit" || code == "limit") return IncidentOutcome::kRecentLimit;
  if (code == "incident_daily_limit") return IncidentOutcome::kDailyLimit;
  if (code == "bad_request" || code == "too_large") return IncidentOutcome::kRefused;
  if (code == "link_lost" || code == "unreachable" || code == "timeout") return IncidentOutcome::kUnreachable;
  return IncidentOutcome::kServiceError;
}

const char* IncidentLimitTitle(IncidentOutcome outcome) {
  if (outcome == IncidentOutcome::kRecentLimit) return "Too many reports were sent recently";
  if (outcome == IncidentOutcome::kDailyLimit) return "Daily report limit reached";
  return nullptr;
}

bool IsIncidentTicket(std::string_view text) {
  constexpr std::string_view kPrefix = "TMNT-";
  constexpr std::string_view kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  if (!text.starts_with(kPrefix) || text.size() != kPrefix.size() + 6) return false;
  return std::all_of(text.begin() + kPrefix.size(), text.end(),
                     [&](char c) { return kAlphabet.find(c) != std::string_view::npos; });
}

bool IsIncidentReceipt(std::string_view text) {
  return text.size() == kReceiptHexDigits &&
         std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

TrackedReport TrackedReportOf(const IncidentPackage& package, const IncidentReceipt& receipt) {
  TrackedReport report;
  report.ticket = receipt.ticket;
  report.receipt = receipt.credential;
  report.submitted_at = receipt.submitted_at;
  report.status_updated_at = receipt.submitted_at;
  report.description = package.Value(kDescriptionKey);
  report.log_file = package.Value("log_file");
  report.launcher_version = package.Value("launcher_version");
  report.guest = package.Value("guest");
  report.platform = package.Value("platform");
  report.operating_system = package.Value("operating_system");
  return report;
}

std::string IncidentStatusRequest(const std::vector<TrackedReport>& reports) {
  std::string list;
  for (size_t i = 0; i < std::min<size_t>(reports.size(), kMostTrackedReports); ++i) {
    if (!list.empty()) list += ',';
    list += "{\"ticket\":" + JsonQuote(reports[i].ticket) + ",\"receipt\":" + JsonQuote(reports[i].receipt) + "}";
  }
  return ",\"reports\":[" + list + "]";
}

std::string IncidentReadRequest(const TrackedReport& report) {
  return ",\"ticket\":" + JsonQuote(report.ticket) + ",\"receipt\":" + JsonQuote(report.receipt);
}

std::string IncidentFileMessage(int seq, const IncidentPackage& package) {
  return Envelope(kIncidentOperation, seq, IncidentRequestFields(package));
}

std::string IncidentStatusMessage(int seq, const std::vector<TrackedReport>& reports) {
  return Envelope(kIncidentStatusOperation, seq, IncidentStatusRequest(reports));
}

std::string IncidentReadMessage(int seq, const TrackedReport& report) {
  return Envelope(kIncidentReadOperation, seq, IncidentReadRequest(report));
}

std::vector<ReportStatus> ReadReportStatuses(std::string_view answer) {
  const std::optional<json::Value> root = json::Parse(answer);
  return root && root->IsObject() ? StatusesOf(*root) : std::vector<ReportStatus>{};
}

IncidentServiceConfig IncidentServiceFromEnvironment() {
  IncidentServiceConfig config;
  config.url = online::kDefaultServiceUrl;
  if (const char* url = std::getenv(kOnlineServiceEnv); url && *url) config.url = url;
  return config;
}

IncidentReceipt FileIncident(const IncidentServiceConfig& config, const fs::path& user_dir, const IncidentPackage& package) {
  Say(config, "DEBUG_SAULO INCIDENT endpoint=" + EndpointHost(config.url));
  const online::Identity identity = online::LoadOrCreateIdentity(user_dir);
  if (!identity.ok()) {
    Say(config, "Bug report: no online identity (" + identity.error + ")");
    return {};
  }
  ServiceSession session;
  std::string error, code;
  if (!session.Connect(config.url, &error)) {
    Say(config, "Bug report: no session: " + error);
    return NotSent(IncidentOutcome::kUnreachable);
  }
  const std::optional<json::Value> greeted = session.Call("hello", HelloBody(identity), &code);
  if (!greeted) {
    Say(config, "Bug report: no session: " + code);
    return NotSent(IncidentOutcomeOf(code));
  }
  if (code != "ok") {
    Say(config, "Bug report: session refused (" + code + ")");
    return NotSent(IncidentOutcomeOf(code));
  }
  const json::Value* capabilities = greeted->Find("capabilities");
  const bool supported = capabilities && capabilities->IsObject() && BoolOf(capabilities->Find("bug_reports"), false);
  Say(config, "DEBUG_SAULO INCIDENT hello bug_reports=" + std::string(supported ? "1" : "0") +
                  " worker_build=" + WorkerBuild(capabilities));
  if (!supported) {
    Say(config, "Bug report: the service takes no reports");
    session.Close();
    return NotSent(IncidentOutcome::kNotSupported);
  }
  const int seq = session.TakeSeq();
  const std::string message = IncidentFileMessage(seq, package);
  const std::optional<json::Value> parsed = json::Parse(message);
  const bool valid = parsed && parsed->IsObject();
  Say(config, "DEBUG_SAULO INCIDENT request op=incident.file seq=" + std::to_string(seq) +
                  " bytes=" + std::to_string(message.size()) + " json_valid=" + (valid ? "1" : "0"));
  Say(config, "DEBUG_SAULO INCIDENT request keys=" + RequestKeys(package));
  if (!valid) {
    Say(config, "Bug report: the report request was not valid JSON");
    session.Close();
    return NotSent(IncidentOutcome::kServiceError);
  }
  const std::optional<json::Value> answer = session.CallRaw(message, seq, &code);
  session.Close();
  Say(config, "DEBUG_SAULO INCIDENT response code=" + code + " seq=" + std::to_string(seq));
  if (!answer || code != "ok") {
    Say(config, "Bug report: not accepted (" + code + ")");
    return NotSent(IncidentOutcomeOf(code));
  }
  const std::string ticket = StringOf(answer->Find("ticket"));
  if (!IsIncidentTicket(ticket)) {
    Say(config, "Bug report: the service sent no valid report id");
    return NotSent(IncidentOutcome::kServiceError);
  }
  const std::string receipt = StringOf(answer->Find("receipt"));
  if (!IsIncidentReceipt(receipt)) {
    Say(config, "Bug report: the service sent no valid receipt");
    return NotSent(IncidentOutcome::kServiceError);
  }
  Say(config, "DEBUG_SAULO INCIDENT ticket=" + ticket);
  Say(config, "Bug report: sent as " + ticket);
  return {IncidentOutcome::kSent, ticket, receipt, Int64Of(answer->Find("submitted_at"))};
}

std::optional<std::vector<ReportStatus>> FetchReportStatuses(const IncidentServiceConfig& config,
                                                             const std::vector<TrackedReport>& reports) {
  ServiceSession session;
  std::string error, code;
  if (!session.Connect(config.url, &error)) return std::nullopt;
  const int seq = session.TakeSeq();
  const std::optional<json::Value> answer = session.CallRaw(IncidentStatusMessage(seq, reports), seq, &code);
  session.Close();
  if (!answer || code != "ok") return std::nullopt;
  const json::Value* list = answer->Find("reports");
  if (!list || !list->IsArray()) return std::nullopt;
  return StatusesOf(*answer);
}

std::optional<IncidentPackage> FetchSentReport(const IncidentServiceConfig& config, const TrackedReport& report) {
  ServiceSession session;
  std::string error, code;
  if (!session.Connect(config.url, &error)) return std::nullopt;
  const int seq = session.TakeSeq();
  const std::optional<json::Value> answer = session.CallRaw(IncidentReadMessage(seq, report), seq, &code);
  session.Close();
  if (!answer || code != "ok") return std::nullopt;
  const json::Value* report_json = answer->Find("report");
  if (!report_json || !report_json->IsObject()) return std::nullopt;
  return ReportOf(report_json);
}

}  // namespace tmnt
