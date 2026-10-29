#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "local_stats.h"

namespace tmnt::live {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNoMoreFiles = 0x12;
constexpr uint32_t kErrorInvalidParameter = 0x57;
constexpr uint32_t kErrorInsufficientBuffer = 0x7A;
constexpr uint32_t kErrorIoPending = 0x3E5;

// Guest layouts: XUSER_STATS_READ_RESULTS, XUSER_STATS_VIEW, XUSER_STATS_ROW, XUSER_STATS_COLUMN, XUSER_STATS_SPEC.
constexpr uint32_t kResultsHeaderSize = 8;
constexpr uint32_t kViewSize = 16;
constexpr uint32_t kRowSize = 48;
constexpr uint32_t kColumnSize = 24;
constexpr uint32_t kSpecHeaderSize = 8;
constexpr uint32_t kSpecMaxColumns = 64;
constexpr uint32_t kSpecSize = kSpecHeaderSize + 2 * kSpecMaxColumns;
constexpr uint32_t kMaxSpecs = 64;

struct LocalUser {
  uint64_t xuid = 0;
  std::string name;
};

struct ViewSpec {
  uint32_t id = 0;
  std::vector<uint16_t> columns;
};

struct StatsRow {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string gamertag;
  std::vector<std::pair<uint16_t, StatColumn>> columns;
};

struct StatsView {
  uint32_t id = 0;
  uint32_t total_rows = 0;
  std::vector<StatsRow> rows;
};

// What an offline console knows: the local player's stats in each view and nobody else.
class LocalLeaderboard {
 public:
  LocalLeaderboard(const LocalStats& stats, LocalUser user) : stats_(stats), user_(std::move(user)) {}

  const LocalUser& user() const { return user_; }

  StatsView Ranked(const ViewSpec& spec, uint32_t first_rank, uint32_t max_rows) const;
  StatsView Around(const ViewSpec& spec, uint64_t xuid, uint32_t max_rows) const;
  StatsView Selected(const ViewSpec& spec, const std::vector<uint64_t>& xuids) const;

 private:
  std::optional<StatsRow> Row(const ViewSpec& spec) const;

  const LocalStats& stats_;
  LocalUser user_;
};

struct StatsQuery {
  enum class Kind { kRanked, kByXuid, kFriends };
  Kind kind = Kind::kRanked;
  uint64_t pivot = 0;
  uint32_t max_rows = 0;
  std::vector<ViewSpec> views;
};

// Handles for enumerators the port answers itself. The range never overlaps the runtime's object handles
// (0xF8xxxxxx), and a handle is retired once its one enumeration has been served.
class EnumeratorRegistry {
 public:
  static constexpr uint32_t kHandleBase = 0xEC000000;
  static constexpr size_t kMaxPending = 64;

  uint32_t Add(StatsQuery query);
  std::optional<StatsQuery> Take(uint32_t handle);
  size_t pending() const;

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, StatsQuery> queries_;
  std::deque<uint32_t> order_;
  uint32_t next_ = 1;
};

uint32_t StatsResultsSize(const std::vector<StatsView>& views);
bool WriteStatsResults(uint8_t* base, uint32_t buffer, uint32_t capacity, const std::vector<StatsView>& views);

constexpr uint32_t HresultFromWin32(uint32_t error) { return error ? 0x80070000 | (error & 0xFFFF) : 0; }

// The game tells failures apart by the extended error, an HRESULT: 0x80070012 is its "no more friends".
void CompleteOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result, uint32_t items);

std::vector<ViewSpec> ReadSpecs(const uint8_t* base, uint32_t specs, uint32_t count);

struct ViewWrite {
  uint32_t view = 0;
  ViewStats stats;
};
// XSESSION_VIEW_PROPERTIES[count] of XUSER_PROPERTY: the last 64-bit property of a view is its rating. Every
// property is also a column, keyed by the low 16 bits of its id, so a read can ask for the rating as a column.
std::vector<ViewWrite> ParseViewWrites(const uint8_t* base, uint32_t views, uint32_t count);

// The game's leaderboard registry: count at +0, 180-byte entries at +8. An entry holds the view id at +0, the
// property count at +8 and the XUSER_PROPERTY array at +20, filled by the script before any session write.
constexpr uint32_t kRegisteredViewSize = 180;
std::optional<ViewWrite> ParseRegisteredView(const uint8_t* base, uint32_t registry, uint32_t handle);
std::vector<ViewWrite> ParseRegisteredViews(const uint8_t* base, uint32_t registry, uint32_t handles, uint32_t count);

// XamUserCreateStatsEnumerator: validates, registers the query and writes the result size and the handle.
// Returns the status for r3.
uint32_t CreateEnumerator(uint8_t* base, EnumeratorRegistry& registry, StatsQuery::Kind kind, uint64_t pivot,
                          uint32_t max_rows, uint32_t spec_count, uint32_t specs, uint32_t size_ptr,
                          uint32_t handle_ptr);

// XFriendsCreateEnumerator: an offline player has no friends, so its one enumeration ends at once.
uint32_t CreateFriendsEnumerator(uint8_t* base, EnumeratorRegistry& registry, uint32_t size_ptr, uint32_t handle_ptr);

std::vector<StatsView> AnswerQuery(const LocalLeaderboard& board, const StatsQuery& query);

// XamEnumerate on a registered handle: fills the results. With an overlapped, it is completed and the call
// returns IO_PENDING like the console does.
uint32_t ServeEnumeration(uint8_t* base, const LocalLeaderboard& board, const StatsQuery& query, uint32_t buffer,
                          uint32_t size, uint32_t items_ptr, uint32_t overlapped);

constexpr uint32_t kMaxReadXuids = 101;

std::vector<StatsView> AnswerReadStats(const uint8_t* base, const LocalLeaderboard& board, uint32_t xuid_count,
                                       uint32_t xuids, uint32_t spec_count, uint32_t specs);

// XUserReadStats: fills the results buffer. False when it is missing or too small.
bool ServeReadStats(uint8_t* base, const LocalLeaderboard& board, uint32_t xuid_count, uint32_t xuids,
                    uint32_t spec_count, uint32_t specs, uint32_t size, uint32_t results);

}  // namespace tmnt::live
