#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace tmnt {

// <user_data_root>/<title id>/live/stats.toml
std::filesystem::path StatsFilePath(const std::filesystem::path& user_data_root, uint32_t title_id);

enum class LoadResult { kMissing, kLoaded, kCorrupt };
enum class RecordResult { kUnchanged, kSaved, kNotSaved };

// XUSER_DATA types a stat column can hold: 1 int32, 2 int64, 3 double, 5 float. Floating values are kept as raw bits.
constexpr uint8_t kTypeInt32 = 1;
constexpr uint8_t kTypeInt64 = 2;
constexpr uint8_t kTypeDouble = 3;
constexpr uint8_t kTypeFloat = 5;

inline bool IsStatType(uint8_t type) {
  return type == kTypeInt32 || type == kTypeInt64 || type == kTypeDouble || type == kTypeFloat;
}

struct StatColumn {
  uint8_t type = 0;
  int64_t bits = 0;
  bool operator==(const StatColumn&) const = default;
};

// What the game submitted for one leaderboard view: the rating that ranks the player plus its other columns.
struct ViewStats {
  int64_t rating = 0;
  std::map<uint16_t, StatColumn> columns;
  bool operator==(const ViewStats&) const = default;
};

// The best submission per user and leaderboard view. The game decides what is worth submitting; the store keeps
// the highest rating so a lower run can never replace a better one.
class LocalStats {
 public:
  static constexpr uint32_t kMaxUsers = 8;
  static constexpr uint32_t kMaxViewsPerUser = 64;
  static constexpr uint32_t kMaxColumns = 16;

  explicit LocalStats(std::filesystem::path file) : file_(std::move(file)) {}

  // A file that cannot be parsed is moved aside to <file>.corrupt and the store starts empty.
  LoadResult Load();

  std::optional<ViewStats> Get(uint64_t xuid, uint32_t view) const;

  // Ratings of zero or less, unknown column types and submissions beyond the limits are ignored.
  RecordResult Record(uint64_t xuid, uint32_t view, const ViewStats& stats);

 private:
  using Views = std::map<uint32_t, ViewStats>;

  bool Parse(std::string_view text, std::map<uint64_t, Views>& out) const;
  bool Save() const;

  std::filesystem::path file_;
  mutable std::mutex mutex_;
  std::map<uint64_t, Views> stats_;
};

}  // namespace tmnt
