#include "live_stats.h"

#include <algorithm>
#include <cstring>

#include "guest_memory.h"

namespace tmnt::live {
namespace {

constexpr uint32_t kMaxWrittenViews = 16;
constexpr uint32_t kMaxProperties = 32;
constexpr uint32_t kViewPropertiesSize = 12;
constexpr uint32_t kPropertySize = 24;
constexpr size_t kGamertagSize = 16;

std::optional<StatColumn> ReadPropertyValue(const uint8_t* base, uint32_t property) {
  const uint8_t type = base[property + 8];
  const uint32_t value = property + 16;
  switch (type) {
    case kTypeInt32:
      return StatColumn{type, static_cast<int32_t>(LoadBE32(base, value))};
    case kTypeFloat:
      return StatColumn{type, static_cast<int64_t>(LoadBE32(base, value))};
    case kTypeInt64:
    case kTypeDouble:
      return StatColumn{type, static_cast<int64_t>(LoadBE64(base, value))};
    default:
      return std::nullopt;
  }
}

void StoreColumn(uint8_t* base, uint32_t at, uint16_t id, const StatColumn& column) {
  StoreBE16(base, at, id);
  base[at + 8] = column.type;
  if (column.type == kTypeInt32 || column.type == kTypeFloat) {
    StoreBE32(base, at + 16, static_cast<uint32_t>(column.bits));
  } else {
    StoreBE64(base, at + 16, static_cast<uint64_t>(column.bits));
  }
}

uint32_t EnumerationSize(const std::vector<ViewSpec>& specs) {
  uint32_t size = kResultsHeaderSize;
  for (const ViewSpec& spec : specs) {
    size += kViewSize + kRowSize + kColumnSize * static_cast<uint32_t>(spec.columns.size());
  }
  return size;
}

}  // namespace

std::optional<StatsRow> LocalLeaderboard::Row(const ViewSpec& spec) const {
  const auto stats = stats_.Get(user_.xuid, spec.id);
  if (!stats) return std::nullopt;
  StatsRow row{user_.xuid, 1, stats->rating, user_.name, {}};
  for (const uint16_t id : spec.columns) {
    const auto column = stats->columns.find(id);
    if (column != stats->columns.end()) row.columns.emplace_back(id, column->second);
  }
  return row;
}

StatsView LocalLeaderboard::Ranked(const ViewSpec& spec, uint32_t first_rank, uint32_t max_rows) const {
  StatsView result;
  result.id = spec.id;
  const auto row = Row(spec);
  if (!row) return result;
  result.total_rows = 1;
  if (first_rank <= 1 && max_rows >= 1) result.rows.push_back(*row);
  return result;
}

StatsView LocalLeaderboard::Around(const ViewSpec& spec, uint64_t xuid, uint32_t max_rows) const {
  if (xuid != user_.xuid) {
    StatsView result;
    result.id = spec.id;
    return result;
  }
  return Ranked(spec, 1, max_rows);
}

// XUserReadStats is a lookup, not a search: the game sizes its results buffer assuming one row per requested
// XUID, so the local player's row must always be present, zero-valued when no score is recorded yet.
StatsView LocalLeaderboard::Selected(const ViewSpec& spec, const std::vector<uint64_t>& xuids) const {
  StatsView result;
  result.id = spec.id;
  if (std::find(xuids.begin(), xuids.end(), user_.xuid) != xuids.end()) {
    const auto stats = stats_.Get(user_.xuid, spec.id);
    StatsRow row{user_.xuid, 1, stats ? stats->rating : 0, user_.name, {}};
    for (const uint16_t id : spec.columns) {
      const auto column = stats ? stats->columns.find(id) : decltype(stats->columns)::const_iterator{};
      row.columns.emplace_back(id, stats && column != stats->columns.end() ? column->second
                                                                            : StatColumn{kTypeInt64, 0});
    }
    result.rows.push_back(std::move(row));
  }
  result.total_rows = static_cast<uint32_t>(result.rows.size());
  return result;
}

uint32_t EnumeratorRegistry::Add(StatsQuery query) {
  std::lock_guard lock(mutex_);
  uint32_t handle;
  do {
    handle = kHandleBase | (next_++ & 0x00FFFFFF);
  } while (queries_.contains(handle));
  queries_.emplace(handle, std::move(query));
  order_.push_back(handle);
  while (queries_.size() > kMaxPending) {
    queries_.erase(order_.front());
    order_.pop_front();
  }
  return handle;
}

std::optional<StatsQuery> EnumeratorRegistry::Take(uint32_t handle) {
  std::lock_guard lock(mutex_);
  const auto it = queries_.find(handle);
  if (it == queries_.end()) return std::nullopt;
  StatsQuery query = std::move(it->second);
  queries_.erase(it);
  order_.erase(std::remove(order_.begin(), order_.end(), handle), order_.end());
  return query;
}

size_t EnumeratorRegistry::pending() const {
  std::lock_guard lock(mutex_);
  return queries_.size();
}

uint32_t StatsResultsSize(const std::vector<StatsView>& views) {
  uint32_t size = kResultsHeaderSize + kViewSize * static_cast<uint32_t>(views.size());
  for (const auto& view : views) {
    for (const auto& row : view.rows) size += kRowSize + kColumnSize * static_cast<uint32_t>(row.columns.size());
  }
  return size;
}

bool WriteStatsResults(uint8_t* base, uint32_t buffer, uint32_t capacity, const std::vector<StatsView>& views) {
  const uint32_t needed = StatsResultsSize(views);
  if (!buffer || capacity < needed) return false;
  std::memset(base + buffer, 0, needed);

  StoreBE32(base, buffer, static_cast<uint32_t>(views.size()));
  StoreBE32(base, buffer + 4, buffer + kResultsHeaderSize);
  uint32_t view_at = buffer + kResultsHeaderSize;
  uint32_t row_at = view_at + kViewSize * static_cast<uint32_t>(views.size());
  uint32_t column_at = row_at;
  for (const auto& view : views) column_at += kRowSize * static_cast<uint32_t>(view.rows.size());

  for (const auto& view : views) {
    StoreBE32(base, view_at, view.id);
    StoreBE32(base, view_at + 4, view.total_rows);
    StoreBE32(base, view_at + 8, static_cast<uint32_t>(view.rows.size()));
    StoreBE32(base, view_at + 12, view.rows.empty() ? 0 : row_at);
    for (const auto& row : view.rows) {
      StoreBE64(base, row_at, row.xuid);
      StoreBE32(base, row_at + 8, row.rank);
      StoreBE64(base, row_at + 16, static_cast<uint64_t>(row.rating));
      std::memcpy(base + row_at + 24, row.gamertag.data(), std::min(row.gamertag.size(), kGamertagSize - 1));
      StoreBE32(base, row_at + 40, static_cast<uint32_t>(row.columns.size()));
      StoreBE32(base, row_at + 44, row.columns.empty() ? 0 : column_at);
      for (const auto& [id, column] : row.columns) {
        StoreColumn(base, column_at, id, column);
        column_at += kColumnSize;
      }
      row_at += kRowSize;
    }
    view_at += kViewSize;
  }
  return true;
}

void CompleteOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result, uint32_t items) {
  StoreBE32(base, overlapped + 0x18, HresultFromWin32(result));
  StoreBE32(base, overlapped + 4, items);
  StoreBE32(base, overlapped, result);
}

std::vector<ViewSpec> ReadSpecs(const uint8_t* base, uint32_t specs, uint32_t count) {
  std::vector<ViewSpec> result;
  for (uint32_t i = 0; i < count && i < kMaxSpecs; ++i) {
    const uint32_t spec = specs + i * kSpecSize;
    ViewSpec view;
    view.id = LoadBE32(base, spec);
    const uint32_t columns = std::min(LoadBE32(base, spec + 4), kSpecMaxColumns);
    for (uint32_t j = 0; j < columns; ++j) view.columns.push_back(LoadBE16(base, spec + kSpecHeaderSize + 2 * j));
    result.push_back(std::move(view));
  }
  return result;
}

namespace {

std::optional<ViewWrite> ParseView(const uint8_t* base, uint32_t view, uint32_t property_count, uint32_t properties) {
  if (!properties || property_count > kMaxProperties) return std::nullopt;

  std::vector<std::pair<uint32_t, StatColumn>> values;
  int rating_index = -1;
  int fallback_rating = -1;
  for (uint32_t j = 0; j < property_count; ++j) {
    const uint32_t property = properties + j * kPropertySize;
    const auto value = ReadPropertyValue(base, property);
    if (!value) continue;
    const uint32_t id = LoadBE32(base, property);
    if (value->type == kTypeInt64) {
      fallback_rating = static_cast<int>(values.size());
      if ((id & 0xFFFF) == 1) rating_index = static_cast<int>(values.size());
    }
    values.emplace_back(id, *value);
  }
  if (rating_index < 0) rating_index = fallback_rating;
  if (rating_index < 0) return std::nullopt;

  ViewWrite write;
  write.view = view;
  write.stats.rating = values[rating_index].second.bits;
  for (const auto& [id, value] : values) write.stats.columns.emplace(static_cast<uint16_t>(id), value);
  return write;
}

}  // namespace

std::vector<ViewWrite> ParseViewWrites(const uint8_t* base, uint32_t views, uint32_t count) {
  std::vector<ViewWrite> writes;
  if (!views || count > kMaxWrittenViews) return writes;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = views + i * kViewPropertiesSize;
    if (auto write = ParseView(base, LoadBE32(base, entry), LoadBE32(base, entry + 4), LoadBE32(base, entry + 8))) {
      writes.push_back(std::move(*write));
    }
  }
  return writes;
}

std::optional<ViewWrite> ParseRegisteredView(const uint8_t* base, uint32_t registry, uint32_t handle) {
  if (!registry) return std::nullopt;
  const uint32_t entries = LoadBE32(base, registry + 8);
  if (!entries || handle >= LoadBE32(base, registry)) return std::nullopt;
  const uint32_t entry = entries + handle * kRegisteredViewSize;
  return ParseView(base, LoadBE32(base, entry), LoadBE32(base, entry + 8), LoadBE32(base, entry + 20));
}

std::vector<ViewWrite> ParseRegisteredViews(const uint8_t* base, uint32_t registry, uint32_t handles, uint32_t count) {
  std::vector<ViewWrite> writes;
  if (!handles || count > kMaxWrittenViews) return writes;
  for (uint32_t i = 0; i < count; ++i) {
    if (auto write = ParseRegisteredView(base, registry, LoadBE32(base, handles + i * 4))) {
      writes.push_back(std::move(*write));
    }
  }
  return writes;
}

uint32_t CreateEnumerator(uint8_t* base, EnumeratorRegistry& registry, StatsQuery::Kind kind, uint64_t pivot,
                          uint32_t max_rows, uint32_t spec_count, uint32_t specs, uint32_t size_ptr,
                          uint32_t handle_ptr) {
  if (!spec_count || spec_count > kMaxSpecs || !specs || !size_ptr || !handle_ptr) return kErrorInvalidParameter;

  StatsQuery query;
  query.kind = kind;
  query.pivot = pivot;
  query.max_rows = max_rows;
  query.views = ReadSpecs(base, specs, spec_count);
  const uint32_t size = EnumerationSize(query.views);
  const uint32_t handle = registry.Add(std::move(query));

  StoreBE32(base, size_ptr, size);
  StoreBE32(base, handle_ptr, handle);
  return kErrorSuccess;
}

uint32_t CreateFriendsEnumerator(uint8_t* base, EnumeratorRegistry& registry, uint32_t size_ptr, uint32_t handle_ptr) {
  if (!size_ptr || !handle_ptr) return kErrorInvalidParameter;
  StatsQuery query;
  query.kind = StatsQuery::Kind::kFriends;
  const uint32_t handle = registry.Add(std::move(query));
  StoreBE32(base, size_ptr, 0);
  StoreBE32(base, handle_ptr, handle);
  return kErrorSuccess;
}

std::vector<StatsView> AnswerQuery(const LocalLeaderboard& board, const StatsQuery& query) {
  std::vector<StatsView> views;
  if (query.kind == StatsQuery::Kind::kFriends) return views;
  for (const ViewSpec& spec : query.views) {
    views.push_back(query.kind == StatsQuery::Kind::kRanked
                        ? board.Ranked(spec, static_cast<uint32_t>(query.pivot), query.max_rows)
                        : board.Around(spec, query.pivot, query.max_rows));
  }
  return views;
}

uint32_t ServeEnumeration(uint8_t* base, const LocalLeaderboard& board, const StatsQuery& query, uint32_t buffer,
                          uint32_t size, uint32_t items_ptr, uint32_t overlapped) {
  uint32_t result = kErrorNoMoreFiles;
  uint32_t items = 0;
  if (query.kind != StatsQuery::Kind::kFriends) {
    result = WriteStatsResults(base, buffer, size, AnswerQuery(board, query)) ? kErrorSuccess : kErrorInsufficientBuffer;
    items = result == kErrorSuccess ? 1 : 0;
  }
  if (items_ptr) StoreBE32(base, items_ptr, items);
  if (!overlapped) return result;
  CompleteOverlapped(base, overlapped, result, items);
  return kErrorIoPending;
}

std::vector<StatsView> AnswerReadStats(const uint8_t* base, const LocalLeaderboard& board, uint32_t xuid_count,
                                       uint32_t xuids, uint32_t spec_count, uint32_t specs) {
  std::vector<uint64_t> requested;
  if (xuids) {
    for (uint32_t i = 0; i < xuid_count && i < kMaxReadXuids; ++i) requested.push_back(LoadBE64(base, xuids + i * 8));
  }
  std::vector<StatsView> views;
  for (const ViewSpec& spec : ReadSpecs(base, specs, spec_count)) views.push_back(board.Selected(spec, requested));
  return views;
}

bool ServeReadStats(uint8_t* base, const LocalLeaderboard& board, uint32_t xuid_count, uint32_t xuids,
                    uint32_t spec_count, uint32_t specs, uint32_t size, uint32_t results) {
  if (!results || !specs || !spec_count || spec_count > kMaxSpecs) return false;
  return WriteStatsResults(base, results, size, AnswerReadStats(base, board, xuid_count, xuids, spec_count, specs));
}

}  // namespace tmnt::live
