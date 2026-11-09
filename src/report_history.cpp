#include "report_history.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include "incident_json.h"
#include "incident_report.h"
#include "localization/ui_text.h"
#include "updater/json.h"

namespace tmnt {
namespace fs = std::filesystem;
namespace json = trg::update::json;
namespace {

constexpr int kHistoryVersion = 1;
constexpr size_t kPreviewCharacters = 60;

constexpr std::string_view kStatuses[][2] = {
    {"new", "New"},
    {"investigating", "Investigating"},
    {"fixed", "Fixed"},
    {"cannot_reproduce", "Cannot reproduce"},
    {"duplicate", "Duplicate"},
    {"not_a_bug", "Not a bug"},
    {"closed", "Closed"},
};

void Member(std::string& out, std::string_view key, const std::string& value, bool comma = true) {
  out += JsonQuote(key);
  out += ':';
  out += JsonQuote(value);
  if (comma) out += ',';
}

std::string EntryJson(const TrackedReport& report) {
  std::string out = "{";
  Member(out, "ticket", report.ticket);
  Member(out, "receipt", report.receipt);
  out += "\"submitted_at\":" + std::to_string(report.submitted_at) + ",";
  Member(out, "description", report.description);
  Member(out, "log_file", report.log_file);
  Member(out, "launcher_version", report.launcher_version);
  Member(out, "guest", report.guest);
  Member(out, "platform", report.platform);
  Member(out, "operating_system", report.operating_system);
  Member(out, "status", report.status);
  out += "\"status_updated_at\":" + std::to_string(report.status_updated_at) + ",";
  Member(out, "fixed_version", report.fixed_version);
  Member(out, "player_message", report.player_message);
  out += "\"unseen\":";
  out += report.unseen ? "true" : "false";
  out += ",\"message_unseen\":";
  out += report.message_unseen ? "true" : "false";
  out += ",\"unavailable\":";
  out += report.unavailable ? "true" : "false";
  return out + "}";
}

std::string StringOf(const json::Value* value) { return value && value->IsString() ? value->AsString() : std::string(); }
bool BoolOf(const json::Value* value) { return value && value->IsBool() && value->AsBool(); }
int64_t IntOf(const json::Value* value) {
  const auto number = value && value->IsNumber() ? value->AsInt64() : std::nullopt;
  return number ? *number : 0;
}

TrackedReport EntryOf(const json::Value& entry) {
  TrackedReport report;
  report.ticket = StringOf(entry.Find("ticket"));
  report.receipt = StringOf(entry.Find("receipt"));
  report.submitted_at = IntOf(entry.Find("submitted_at"));
  report.description = StringOf(entry.Find("description"));
  report.log_file = StringOf(entry.Find("log_file"));
  report.launcher_version = StringOf(entry.Find("launcher_version"));
  report.guest = StringOf(entry.Find("guest"));
  report.platform = StringOf(entry.Find("platform"));
  report.operating_system = StringOf(entry.Find("operating_system"));
  report.status = StringOf(entry.Find("status"));
  report.status_updated_at = IntOf(entry.Find("status_updated_at"));
  report.fixed_version = StringOf(entry.Find("fixed_version"));
  report.player_message = StringOf(entry.Find("player_message"));
  report.unseen = BoolOf(entry.Find("unseen"));
  report.message_unseen = entry.Find("message_unseen") ? BoolOf(entry.Find("message_unseen"))
                                                       : report.unseen && !report.player_message.empty();
  report.message_unseen = report.message_unseen && !report.player_message.empty();
  report.unavailable = BoolOf(entry.Find("unavailable"));
  return report;
}

void NewestFirst(std::vector<TrackedReport>& reports) {
  std::stable_sort(reports.begin(), reports.end(),
                   [](const TrackedReport& a, const TrackedReport& b) { return a.submitted_at > b.submitted_at; });
  if (reports.size() > kMostTrackedReports) reports.resize(kMostTrackedReports);
}

void SetAside(const fs::path& file) {
  fs::path damaged = file;
  damaged += ".damaged";
  std::error_code ec;
  fs::rename(file, damaged, ec);
  std::printf("Bug report: %s is damaged; My reports starts empty\n", file.filename().string().c_str());
}

}  // namespace

const char* ReportStatusText(std::string_view status) {
  const auto found = std::find_if(std::begin(kStatuses), std::end(kStatuses), [&](const auto& pair) { return pair[0] == status; });
  return found == std::end(kStatuses) ? nullptr : (*found)[1].data();
}

std::string ReportStatusLine(const TrackedReport& report) {
  if (report.status == "fixed" && !report.fixed_version.empty()) return std::string(Tr("Fixed in version")) + " " + report.fixed_version;
  const char* text = ReportStatusText(report.status);
  return text ? Tr(text) : report.status;
}

std::string ReportPreview(std::string_view description) {
  std::string line(description.substr(0, BytesOfCharacters(description, kPreviewCharacters)));
  std::replace(line.begin(), line.end(), '\n', ' ');
  return line.size() < description.size() ? line + "..." : line;
}

void ReportHistory::Load() {
  reports_.clear();
  std::error_code ec;
  if (!fs::exists(file_, ec)) return;
  std::string text;
  {
    // Closed before parsing: Windows cannot rename a damaged file while this stream holds it.
    std::ifstream in(file_, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::optional<json::Value> root = json::Parse(text);
  const json::Value* list = root && root->IsObject() ? root->Find("reports") : nullptr;
  if (!list || !list->IsArray()) {
    SetAside(file_);
    return;
  }
  for (const json::Value& entry : list->AsArray()) {
    if (!entry.IsObject()) continue;
    TrackedReport report = EntryOf(entry);
    if (IsIncidentTicket(report.ticket) && IsIncidentReceipt(report.receipt) && ReportStatusText(report.status)) {
      reports_.push_back(std::move(report));
    }
  }
  NewestFirst(reports_);
}

bool ReportHistory::Save() const {
  std::string root = "{\"version\":" + std::to_string(kHistoryVersion) + ",\"reports\":[";
  for (size_t i = 0; i < reports_.size(); ++i) {
    if (i) root += ',';
    root += EntryJson(reports_[i]);
  }
  root += "]}";

  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  fs::path temporary = file_;
  temporary += ".new";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << root;
    out.flush();
    if (!out) {
      out.close();
      fs::remove(temporary, ec);
      return false;
    }
  }
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, ec);
  fs::rename(temporary, file_, ec);
  if (!ec) return true;
  std::error_code ignored;
  fs::remove(temporary, ignored);
  return false;
}

bool ReportHistory::HasUnseen() const {
  return std::any_of(reports_.begin(), reports_.end(), [](const TrackedReport& report) { return report.unseen; });
}

bool ReportHistory::HasUnseenMessage() const {
  return std::any_of(reports_.begin(), reports_.end(), [](const TrackedReport& report) { return report.message_unseen; });
}

std::string ReportHistory::NextUnseen() const {
  const TrackedReport* best = nullptr;
  for (const TrackedReport& report : reports_) {
    if (!report.unseen && !report.message_unseen) continue;
    const bool better = !best || (report.message_unseen && !best->message_unseen) ||
                        (report.message_unseen == best->message_unseen && report.status_updated_at > best->status_updated_at);
    if (better) best = &report;
  }
  return best ? best->ticket : std::string();
}

bool ReportHistory::Add(TrackedReport report) {
  Remove(report.ticket);
  reports_.push_back(std::move(report));
  NewestFirst(reports_);
  return Save();
}

bool ReportHistory::Apply(const std::vector<ReportStatus>& statuses, const std::vector<std::string>& asked) {
  bool changed = false;
  for (const ReportStatus& status : statuses) {
    if (!ReportStatusText(status.status)) continue;
    const auto found = std::find_if(reports_.begin(), reports_.end(), [&](const TrackedReport& r) { return r.ticket == status.ticket; });
    if (found == reports_.end()) continue;
    if (std::exchange(found->unavailable, false)) changed = true;
    if (found->status == status.status && found->status_updated_at == status.updated_at &&
        found->fixed_version == status.fixed_version && found->player_message == status.player_message) {
      continue;
    }
    if (found->player_message != status.player_message) found->message_unseen = !status.player_message.empty();
    found->status = status.status;
    found->status_updated_at = status.updated_at;
    found->fixed_version = status.fixed_version;
    found->player_message = status.player_message;
    found->unseen = true;
    changed = true;
  }
  for (const std::string& ticket : asked) {
    const bool answered = std::any_of(statuses.begin(), statuses.end(), [&](const ReportStatus& s) { return s.ticket == ticket; });
    const auto found = std::find_if(reports_.begin(), reports_.end(), [&](const TrackedReport& r) { return r.ticket == ticket; });
    if (answered || found == reports_.end() || found->unavailable) continue;
    found->unavailable = true;
    changed = true;
  }
  return changed;
}

bool ReportHistory::MarkSeen(std::string_view ticket) {
  const auto found = std::find_if(reports_.begin(), reports_.end(), [&](const TrackedReport& r) { return r.ticket == ticket; });
  if (found == reports_.end() || (!found->unseen && !found->message_unseen)) return false;
  found->unseen = false;
  found->message_unseen = false;
  return true;
}

bool ReportHistory::Remove(std::string_view ticket) {
  return std::erase_if(reports_, [&](const TrackedReport& r) { return r.ticket == ticket; }) > 0;
}

}  // namespace tmnt
