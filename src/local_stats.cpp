#include "local_stats.h"

#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tmnt {
namespace {

constexpr uintmax_t kMaxFileSize = 1u << 20;

std::string_view Trim(std::string_view s) {
  constexpr std::string_view kSpace = " \t\r\n";
  const size_t first = s.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

template <typename T>
bool ParseNumber(std::string_view s, T& out, int base = 10) {
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
  return ec == std::errc() && end == s.data() + s.size();
}

bool ParseColumn(std::string_view value, StatColumn& out) {
  if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
  value = value.substr(1, value.size() - 2);
  const size_t comma = value.find(',');
  if (comma == std::string_view::npos) return false;
  uint32_t type;
  if (!ParseNumber(Trim(value.substr(0, comma)), type) || type > 0xFF || !IsStatType(static_cast<uint8_t>(type))) {
    return false;
  }
  out.type = static_cast<uint8_t>(type);
  return ParseNumber(Trim(value.substr(comma + 1)), out.bits);
}

bool WriteDurably(const std::filesystem::path& path, const std::string& text) {
#if defined(_WIN32)
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0) return false;
#else
  FILE* f = std::fopen(path.c_str(), "wb");
#endif
  if (!f) return false;
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  return std::fclose(f) == 0 && ok;
}

bool Acceptable(const ViewStats& stats) {
  if (stats.rating <= 0 || stats.columns.size() > LocalStats::kMaxColumns) return false;
  for (const auto& [id, column] : stats.columns) {
    if (!IsStatType(column.type)) return false;
  }
  return true;
}

}  // namespace

std::filesystem::path StatsFilePath(const std::filesystem::path& user_data_root, uint32_t title_id) {
  char title[9];
  std::snprintf(title, sizeof(title), "%08" PRIX32, title_id);
  return user_data_root / title / "live" / "stats.toml";
}

LoadResult LocalStats::Load() {
  std::lock_guard lock(mutex_);
  stats_.clear();

  std::error_code ec;
  if (!std::filesystem::is_regular_file(file_, ec)) return LoadResult::kMissing;

  std::string text;
  const uintmax_t size = std::filesystem::file_size(file_, ec);
  bool parsed = !ec && size <= kMaxFileSize;
  if (parsed) {
    std::ifstream in(file_, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    parsed = in.is_open() && !in.bad();
  }
  std::map<uint64_t, Views> loaded;
  if (parsed && Parse(text, loaded)) {
    stats_ = std::move(loaded);
    return LoadResult::kLoaded;
  }

  auto aside = file_;
  aside += ".corrupt";
  std::filesystem::rename(file_, aside, ec);
  return LoadResult::kCorrupt;
}

bool LocalStats::Parse(std::string_view text, std::map<uint64_t, Views>& out) const {
  constexpr std::string_view kSectionPrefix = "[users.";
  constexpr std::string_view kViewInfix = ".view_";
  constexpr std::string_view kColumnPrefix = "col_";

  ViewStats* current = nullptr;
  bool has_version = false;
  std::set<std::pair<uint64_t, uint32_t>> seen;

  for (size_t pos = 0; pos < text.size();) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line.front() == '#') continue;

    if (line.front() == '[') {
      if (!line.starts_with(kSectionPrefix) || line.back() != ']') return false;
      const std::string_view path = line.substr(kSectionPrefix.size(), line.size() - kSectionPrefix.size() - 1);
      if (path.size() <= 16 || !path.substr(16).starts_with(kViewInfix)) return false;
      uint64_t xuid;
      uint32_t view;
      if (!ParseNumber(path.substr(0, 16), xuid, 16) || !ParseNumber(path.substr(16 + kViewInfix.size()), view)) {
        return false;
      }
      if (!seen.emplace(xuid, view).second || !has_version) return false;
      Views& views = out[xuid];
      if (out.size() > kMaxUsers || views.size() >= kMaxViewsPerUser) return false;
      current = &views[view];
      continue;
    }

    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view key = Trim(line.substr(0, eq));
    const std::string_view value = Trim(line.substr(eq + 1));
    if (!current) {
      if (key != "version" || value != "1" || has_version) return false;
      has_version = true;
      continue;
    }

    if (key == "rating") {
      if (!ParseNumber(value, current->rating) || current->rating <= 0) return false;
    } else if (key.starts_with(kColumnPrefix)) {
      uint32_t id;
      StatColumn column;
      if (!ParseNumber(key.substr(kColumnPrefix.size()), id) || id > 0xFFFF || !ParseColumn(value, column) ||
          current->columns.size() >= kMaxColumns) {
        return false;
      }
      if (!current->columns.emplace(static_cast<uint16_t>(id), column).second) return false;
    } else {
      return false;
    }
  }

  for (const auto& [xuid, views] : out) {
    for (const auto& [view, stats] : views) {
      if (stats.rating <= 0) return false;
    }
  }
  return true;
}

std::optional<ViewStats> LocalStats::Get(uint64_t xuid, uint32_t view) const {
  std::lock_guard lock(mutex_);
  const auto user = stats_.find(xuid);
  if (user == stats_.end()) return std::nullopt;
  const auto found = user->second.find(view);
  if (found == user->second.end()) return std::nullopt;
  return found->second;
}

RecordResult LocalStats::Record(uint64_t xuid, uint32_t view, const ViewStats& stats) {
  if (!Acceptable(stats)) return RecordResult::kUnchanged;
  std::lock_guard lock(mutex_);

  const auto user = stats_.find(xuid);
  if (user == stats_.end() && stats_.size() >= kMaxUsers) return RecordResult::kUnchanged;
  if (user != stats_.end()) {
    const auto existing = user->second.find(view);
    if (existing != user->second.end() && existing->second.rating >= stats.rating) return RecordResult::kUnchanged;
    if (existing == user->second.end() && user->second.size() >= kMaxViewsPerUser) return RecordResult::kUnchanged;
  }
  stats_[xuid][view] = stats;
  return Save() ? RecordResult::kSaved : RecordResult::kNotSaved;
}

bool LocalStats::Save() const {
  std::string text =
      "# Offline stats for the local player: the best submission per user and view. No Xbox Live.\n"
      "version = 1\n";
  char section[64];
  for (const auto& [xuid, views] : stats_) {
    for (const auto& [view, stats] : views) {
      std::snprintf(section, sizeof(section), "\n[users.%016" PRIX64 ".view_%" PRIu32 "]\n", xuid, view);
      text += section;
      text += "rating = " + std::to_string(stats.rating) + "\n";
      for (const auto& [id, column] : stats.columns) {
        text += "col_" + std::to_string(id) + " = [" + std::to_string(column.type) + ", " +
                std::to_string(column.bits) + "]\n";
      }
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);
  if (ec) return false;
  auto temporary = file_;
  temporary += ".tmp";
  if (!WriteDurably(temporary, text)) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  std::filesystem::rename(temporary, file_, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

}  // namespace tmnt
