// Tests for the offline Live leaderboard: the local stats store, the guest results buffers and the enumerators.
// Run: tmnt_live_tests

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "live_scope.h"
#include "live_stats.h"
#include "local_stats.h"

using namespace tmnt;
using namespace tmnt::live;

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

constexpr uint64_t kXuid = 0x1234567890ABCDEF;
constexpr uint64_t kOtherXuid = 0x1234567890ABCDE0;

struct TempDir {
  TempDir() {
    path = std::filesystem::temp_directory_path() / ("tmnt_live_tests_" + std::to_string(std::rand()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

std::string ReadAll(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& file, const std::string& text) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out << text;
}

ViewStats Stats(int64_t rating, std::map<uint16_t, StatColumn> columns = {}) {
  return ViewStats{rating, std::move(columns)};
}

struct Guest {
  std::vector<uint8_t> memory = std::vector<uint8_t>(1 << 16, 0xCD);
  uint8_t* base() { return memory.data(); }

  void Spec(uint32_t at, uint32_t view, const std::vector<uint16_t>& columns) {
    StoreBE32(base(), at, view);
    StoreBE32(base(), at + 4, static_cast<uint32_t>(columns.size()));
    for (size_t i = 0; i < columns.size(); ++i) StoreBE16(base(), at + 8 + 2 * static_cast<uint32_t>(i), columns[i]);
  }
  void Property(uint32_t at, uint32_t id, uint8_t type, uint64_t value) {
    std::memset(base() + at, 0, 24);
    StoreBE32(base(), at, id);
    base()[at + 8] = type;
    if (type == kTypeInt32 || type == kTypeFloat) {
      StoreBE32(base(), at + 16, static_cast<uint32_t>(value));
    } else {
      StoreBE64(base(), at + 16, value);
    }
  }
  void View(uint32_t at, uint32_t view, uint32_t count, uint32_t properties) {
    StoreBE32(base(), at, view);
    StoreBE32(base(), at + 4, count);
    StoreBE32(base(), at + 8, properties);
  }
};

struct Board {
  explicit Board(const TempDir& dir) : stats(dir.path / "stats.toml"), board(stats, LocalUser{kXuid, "Player"}) {}
  LocalStats stats;
  LocalLeaderboard board;
};

void TestCleanFileAndFirstValue() {
  std::printf("stats: clean file and first value\n");
  TempDir dir;
  LocalStats stats(dir.path / "live" / "stats.toml");
  CHECK(stats.Load() == LoadResult::kMissing);
  CHECK(!stats.Get(kXuid, 1));
  CHECK(stats.Record(kXuid, 1, Stats(1000, {{1, {kTypeInt64, 1000}}})) == RecordResult::kSaved);
  const auto got = stats.Get(kXuid, 1);
  CHECK(got && got->rating == 1000 && got->columns.at(1).bits == 1000);
  CHECK(std::filesystem::exists(dir.path / "live" / "stats.toml"));
}

void TestRestartKeepsValues() {
  std::printf("stats: restart\n");
  TempDir dir;
  const auto file = dir.path / "stats.toml";
  {
    LocalStats stats(file);
    stats.Load();
    stats.Record(kXuid, 1, Stats(1000, {{1, {kTypeInt64, 1000}}, {2, {kTypeInt32, 2}}}));
    stats.Record(kXuid, 2, Stats(55));
  }
  LocalStats reloaded(file);
  CHECK(reloaded.Load() == LoadResult::kLoaded);
  const auto first = reloaded.Get(kXuid, 1);
  CHECK(first && first->rating == 1000);
  CHECK(first && first->columns.size() == 2 && first->columns.at(2) == (StatColumn{kTypeInt32, 2}));
  CHECK(reloaded.Get(kXuid, 2) && reloaded.Get(kXuid, 2)->rating == 55);
  CHECK(!reloaded.Get(kXuid, 3));
}

void TestBestScoreAndIndependentViews() {
  std::printf("stats: best score and independent views\n");
  TempDir dir;
  LocalStats stats(dir.path / "stats.toml");
  stats.Load();
  stats.Record(kXuid, 1, Stats(100));
  stats.Record(kXuid, 2, Stats(200));
  CHECK(stats.Record(kXuid, 1, Stats(100)) == RecordResult::kUnchanged);
  CHECK(stats.Record(kXuid, 1, Stats(40)) == RecordResult::kUnchanged);
  CHECK(stats.Get(kXuid, 1)->rating == 100);
  CHECK(stats.Record(kXuid, 1, Stats(120)) == RecordResult::kSaved);
  CHECK(stats.Get(kXuid, 1)->rating == 120);
  CHECK(stats.Get(kXuid, 2)->rating == 200);
}

void TestRejectedSubmissions() {
  std::printf("stats: rejected submissions\n");
  TempDir dir;
  LocalStats stats(dir.path / "stats.toml");
  stats.Load();
  CHECK(stats.Record(kXuid, 1, Stats(0)) == RecordResult::kUnchanged);
  CHECK(stats.Record(kXuid, 1, Stats(-5)) == RecordResult::kUnchanged);
  CHECK(stats.Record(kXuid, 1, Stats(5, {{1, {4, 1}}})) == RecordResult::kUnchanged);
  std::map<uint16_t, StatColumn> many;
  for (uint16_t i = 0; i <= LocalStats::kMaxColumns; ++i) many[i] = {kTypeInt32, 1};
  CHECK(stats.Record(kXuid, 1, Stats(5, many)) == RecordResult::kUnchanged);
}

void TestMalformedFiles() {
  std::printf("stats: malformed and hand-edited files\n");
  const char* bad[] = {
      "garbage",
      "version = 2\n",
      "[users.1234567890ABCDEF.view_1]\nrating = 5\n",
      "version = 1\n[users.1234567890ABCDEF.view_1]\nrating = 0\n",
      "version = 1\n[users.1234567890ABCDEF.view_1]\n",
      "version = 1\n[users.1234567890ABCDEF.view_1]\nrating = 5\ncol_1 = [4, 1]\n",
      "version = 1\n[users.1234567890ABCDEF.view_1]\nrating = 5\ncol_70000 = [1, 1]\n",
      "version = 1\n[users.1234567890ABCDEF.view_1]\nrating = 5\nextra = 1\n",
  };
  for (const char* text : bad) {
    TempDir dir;
    const auto file = dir.path / "stats.toml";
    WriteAll(file, text);
    LocalStats stats(file);
    CHECK(stats.Load() == LoadResult::kCorrupt);
    CHECK(!stats.Get(kXuid, 1));
    CHECK(!std::filesystem::exists(file));
    CHECK(std::filesystem::exists(dir.path / "stats.toml.corrupt"));
  }
  {
    TempDir dir;
    const auto file = dir.path / "stats.toml";
    WriteAll(file, "version = 1\n[users.1234567890ABCDEF.view_1]\nrating = 5\ncol_1 = [ 2 , 9 ]\n");
    LocalStats stats(file);
    CHECK(stats.Load() == LoadResult::kLoaded);
    CHECK(stats.Get(kXuid, 1) && stats.Get(kXuid, 1)->columns.at(1).bits == 9);
  }
}

void TestAtomicReplacement() {
  std::printf("stats: atomic replacement\n");
  TempDir dir;
  const auto file = dir.path / "stats.toml";
  LocalStats stats(file);
  stats.Load();
  stats.Record(kXuid, 1, Stats(10));
  const std::string before = ReadAll(file);
  CHECK(!before.empty());
  CHECK(!std::filesystem::exists(dir.path / "stats.toml.tmp"));

  std::filesystem::create_directories(dir.path / "stats.toml.tmp");
  CHECK(stats.Record(kXuid, 1, Stats(20)) == RecordResult::kNotSaved);
  CHECK(ReadAll(file) == before);
  CHECK(stats.Get(kXuid, 1)->rating == 20);
  std::filesystem::remove_all(dir.path / "stats.toml.tmp");
  CHECK(stats.Record(kXuid, 1, Stats(30)) == RecordResult::kSaved);
  LocalStats reloaded(file);
  CHECK(reloaded.Load() == LoadResult::kLoaded && reloaded.Get(kXuid, 1)->rating == 30);
}

void TestPathSafety() {
  std::printf("stats: path stays under the user data root\n");
  const std::filesystem::path root = std::filesystem::path("data") / "user";
  const auto path = StatsFilePath(root, 0x584109D2);
  CHECK(path == root / "584109D2" / "live" / "stats.toml");
  CHECK(StatsFilePath(root, 0x0000001F).parent_path().parent_path().filename() == "0000001F");
}

void TestParseViewWrites() {
  std::printf("writes: rating and columns from the properties\n");
  Guest g;
  g.Property(0x1000, 0x10000007, kTypeInt32, 3);
  g.Property(0x1018, 0x10000008, kTypeFloat, std::bit_cast<uint32_t>(1.5f));
  g.Property(0x1030, 0x10000001, kTypeInt64, 123456);
  g.View(0x2000, 1, 3, 0x1000);
  const auto writes = ParseViewWrites(g.base(), 0x2000, 1);
  CHECK(writes.size() == 1);
  CHECK(writes[0].view == 1 && writes[0].stats.rating == 123456);
  CHECK(writes[0].stats.columns.size() == 3);
  CHECK(writes[0].stats.columns.at(1) == (StatColumn{kTypeInt64, 123456}));
  CHECK(writes[0].stats.columns.at(7) == (StatColumn{kTypeInt32, 3}));
  CHECK(writes[0].stats.columns.at(8).type == kTypeFloat);

  g.Property(0x1000, 0x10000007, kTypeInt32, 0xFFFFFFFE);
  CHECK(ParseViewWrites(g.base(), 0x2000, 1)[0].stats.columns.at(7).bits == -2);

  g.Property(0x1030, 0x10000001, kTypeInt32, 1);
  CHECK(ParseViewWrites(g.base(), 0x2000, 1).empty());
  CHECK(ParseViewWrites(g.base(), 0, 1).empty());
  CHECK(ParseViewWrites(g.base(), 0x2000, 17).empty());
}

void TestEnumeratorCreation() {
  std::printf("enumerator: creation and validation\n");
  Guest g;
  EnumeratorRegistry registry;
  g.Spec(0x1000, 1, {1, 2});
  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 1, 0x1000, 0x3000, 0x3004) ==
        kErrorSuccess);
  const uint32_t handle = LoadBE32(g.base(), 0x3004);
  CHECK(handle >= EnumeratorRegistry::kHandleBase && handle < 0xF8000000);
  CHECK(LoadBE32(g.base(), 0x3000) == kResultsHeaderSize + kViewSize + kRowSize + 2 * kColumnSize);
  CHECK(registry.pending() == 1);

  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 0, 0x1000, 0x3000, 0x3004) ==
        kErrorInvalidParameter);
  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 65, 0x1000, 0x3000, 0x3004) ==
        kErrorInvalidParameter);
  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 1, 0, 0x3000, 0x3004) ==
        kErrorInvalidParameter);
  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 1, 0x1000, 0, 0x3004) ==
        kErrorInvalidParameter);
  CHECK(CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 100, 1, 0x1000, 0x3000, 0) ==
        kErrorInvalidParameter);
}

void TestRegistryRetiresAndBounds() {
  std::printf("enumerator: handles retire and stay bounded\n");
  EnumeratorRegistry registry;
  const uint32_t first = registry.Add({});
  CHECK(registry.Take(first));
  CHECK(!registry.Take(first));
  CHECK(!registry.Take(0xF8000001));
  CHECK(!registry.Take(0));
  CHECK(registry.pending() == 0);
  std::vector<uint32_t> handles;
  for (size_t i = 0; i < EnumeratorRegistry::kMaxPending + 10; ++i) handles.push_back(registry.Add({}));
  CHECK(registry.pending() == EnumeratorRegistry::kMaxPending);
  CHECK(!registry.Take(handles.front()));
  CHECK(registry.Take(handles.back()));
  for (const uint32_t handle : handles) CHECK((handle & 0xFF000000) == EnumeratorRegistry::kHandleBase);
}

void TestEnumerationAnswers() {
  std::printf("enumerator: empty and local answers\n");
  TempDir dir;
  Board b(dir);
  Guest g;
  g.Spec(0x1000, 1, {1, 2, 9});
  StatsQuery ranked;
  ranked.kind = StatsQuery::Kind::kRanked;
  ranked.pivot = 1;
  ranked.max_rows = 100;
  ranked.views = ReadSpecs(g.base(), 0x1000, 1);
  const uint32_t size = kResultsHeaderSize + kViewSize + kRowSize + 3 * kColumnSize;

  CHECK(ServeEnumeration(g.base(), b.board, ranked, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), 0x3000) == 1);
  CHECK(LoadBE32(g.base(), 0x4000) == 1);
  const uint32_t empty_view = LoadBE32(g.base(), 0x4004);
  CHECK(LoadBE32(g.base(), empty_view) == 1 && LoadBE32(g.base(), empty_view + 8) == 0);

  b.stats.Record(kXuid, 1, Stats(5000, {{1, {kTypeInt64, 5000}}, {2, {kTypeInt32, 4}}, {8, {kTypeInt32, 1}}}));
  CHECK(ServeEnumeration(g.base(), b.board, ranked, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  const uint32_t view = LoadBE32(g.base(), 0x4004);
  CHECK(LoadBE32(g.base(), view + 4) == 1 && LoadBE32(g.base(), view + 8) == 1);
  const uint32_t row = LoadBE32(g.base(), view + 12);
  CHECK(row != 0);
  CHECK(LoadBE64(g.base(), row) == kXuid && LoadBE32(g.base(), row + 8) == 1);
  CHECK(LoadBE64(g.base(), row + 16) == 5000);
  CHECK(std::strcmp(reinterpret_cast<const char*>(g.base() + row + 24), "Player") == 0);
  CHECK(LoadBE32(g.base(), row + 40) == 2);
  const uint32_t columns = LoadBE32(g.base(), row + 44);
  CHECK(LoadBE16(g.base(), columns) == 1 && LoadBE64(g.base(), columns + 16) == 5000);
  CHECK(LoadBE16(g.base(), columns + 24) == 2 && LoadBE32(g.base(), columns + 24 + 16) == 4);

  ranked.pivot = 2;
  CHECK(ServeEnumeration(g.base(), b.board, ranked, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), LoadBE32(g.base(), 0x4004) + 8) == 0);

  StatsQuery around = ranked;
  around.kind = StatsQuery::Kind::kByXuid;
  around.pivot = kOtherXuid;
  CHECK(ServeEnumeration(g.base(), b.board, around, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), LoadBE32(g.base(), 0x4004) + 8) == 0);
  around.pivot = kXuid;
  CHECK(ServeEnumeration(g.base(), b.board, around, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), LoadBE32(g.base(), 0x4004) + 8) == 1);

  StatsQuery by_rating_twin = around;
  by_rating_twin.pivot = 5000;
  CHECK(ServeEnumeration(g.base(), b.board, by_rating_twin, 0x4000, size, 0x3000, 0) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), LoadBE32(g.base(), 0x4004) + 8) == 0);
}

void TestEnumerationBufferAndOverlapped() {
  std::printf("enumerator: buffer size and overlapped completion\n");
  TempDir dir;
  Board b(dir);
  b.stats.Record(kXuid, 1, Stats(5000));
  Guest g;
  g.Spec(0x1000, 1, {});
  StatsQuery query;
  query.kind = StatsQuery::Kind::kRanked;
  query.pivot = 1;
  query.max_rows = 10;
  query.views = ReadSpecs(g.base(), 0x1000, 1);
  const uint32_t needed = kResultsHeaderSize + kViewSize + kRowSize;

  StoreBE32(g.base(), 0x3000, 77);
  CHECK(ServeEnumeration(g.base(), b.board, query, 0x4000, needed - 1, 0x3000, 0) == kErrorInsufficientBuffer);
  CHECK(LoadBE32(g.base(), 0x3000) == 0);
  CHECK(ServeEnumeration(g.base(), b.board, query, 0, needed, 0x3000, 0) == kErrorInsufficientBuffer);
  CHECK(ServeEnumeration(g.base(), b.board, query, 0x4000, needed, 0, 0) == kErrorSuccess);

  std::memset(g.base() + 0x5000, 0xAA, 0x20);
  CHECK(ServeEnumeration(g.base(), b.board, query, 0x4000, needed, 0x3000, 0x5000) == kErrorIoPending);
  CHECK(LoadBE32(g.base(), 0x5000) == kErrorSuccess && LoadBE32(g.base(), 0x5004) == 1 &&
        LoadBE32(g.base(), 0x5018) == kErrorSuccess);
  CHECK(ServeEnumeration(g.base(), b.board, query, 0x4000, 4, 0x3000, 0x5000) == kErrorIoPending);
  CHECK(LoadBE32(g.base(), 0x5000) == kErrorInsufficientBuffer && LoadBE32(g.base(), 0x5004) == 0);
  CHECK(LoadBE32(g.base(), 0x5018) == 0x8007007A);

  CHECK(HresultFromWin32(kErrorSuccess) == 0);
  CHECK(HresultFromWin32(kErrorNoMoreFiles) == 0x80070012);
}

void TestFriendsHaveNobody() {
  std::printf("friends: an offline player has none, and the enumeration ends the way the game expects\n");
  TempDir dir;
  Board b(dir);
  b.stats.Record(kXuid, 1, Stats(5000));
  Guest g;
  EnumeratorRegistry registry;
  CHECK(CreateFriendsEnumerator(g.base(), registry, 0, 0x3004) == kErrorInvalidParameter);
  CHECK(CreateFriendsEnumerator(g.base(), registry, 0x3000, 0) == kErrorInvalidParameter);
  CHECK(registry.pending() == 0);

  CHECK(CreateFriendsEnumerator(g.base(), registry, 0x3000, 0x3004) == kErrorSuccess);
  CHECK(LoadBE32(g.base(), 0x3000) == 0);
  const uint32_t handle = LoadBE32(g.base(), 0x3004);
  CHECK((handle & 0xFF000000) == EnumeratorRegistry::kHandleBase);
  const auto query = registry.Take(handle);
  CHECK(query && query->kind == StatsQuery::Kind::kFriends);
  CHECK(!registry.Take(handle).has_value());
  CHECK(AnswerQuery(b.board, *query).empty());

  StoreBE32(g.base(), 0x3008, 77);
  CHECK(ServeEnumeration(g.base(), b.board, *query, 0, 0, 0x3008, 0) == kErrorNoMoreFiles);
  CHECK(LoadBE32(g.base(), 0x3008) == 0);

  std::memset(g.base() + 0x5000, 0xAA, 0x20);
  CHECK(ServeEnumeration(g.base(), b.board, *query, 0, 0, 0, 0x5000) == kErrorIoPending);
  CHECK(LoadBE32(g.base(), 0x5000) == kErrorNoMoreFiles && LoadBE32(g.base(), 0x5004) == 0);
  CHECK(LoadBE32(g.base(), 0x5018) == 0x80070012);
}

void TestReadStats() {
  std::printf("read stats: the local self-row is always present, zero-valued until a score exists\n");
  TempDir dir;
  Board b(dir);
  Guest g;
  g.Spec(0x1000, 1, {1});
  g.Spec(0x1000 + kSpecSize, 2, {1});
  StoreBE64(g.base(), 0x1800, kOtherXuid);
  StoreBE64(g.base(), 0x1808, kXuid);

  // Nobody has a score yet: the game pre-sizes its buffer assuming one row per requested XUID, so the local
  // row must still be written, zero-valued, never omitted.
  CHECK(ServeReadStats(g.base(), b.board, 2, 0x1800, 1, 0x1000, 4096, 0x4000));
  CHECK(LoadBE32(g.base(), 0x4000) == 1);
  uint32_t view = LoadBE32(g.base(), 0x4004);
  CHECK(LoadBE32(g.base(), view) == 1 && LoadBE32(g.base(), view + 4) == 1 && LoadBE32(g.base(), view + 8) == 1);
  uint32_t row = LoadBE32(g.base(), view + 12);
  CHECK(row != 0);
  CHECK(LoadBE64(g.base(), row) == kXuid && LoadBE32(g.base(), row + 8) == 1 && LoadBE64(g.base(), row + 16) == 0);
  CHECK(LoadBE32(g.base(), row + 40) == 1);
  uint32_t columns = LoadBE32(g.base(), row + 44);
  CHECK(columns != 0 && LoadBE16(g.base(), columns) == 1);
  CHECK(g.base()[columns + 8] == kTypeInt64 && LoadBE64(g.base(), columns + 16) == 0);

  b.stats.Record(kXuid, 2, Stats(700, {{1, {kTypeInt64, 700}}}));
  CHECK(ServeReadStats(g.base(), b.board, 2, 0x1800, 2, 0x1000, 4096, 0x4000));
  CHECK(LoadBE32(g.base(), 0x4000) == 2);
  view = LoadBE32(g.base(), 0x4004);
  CHECK(LoadBE32(g.base(), view) == 1 && LoadBE32(g.base(), view + 8) == 1);
  CHECK(LoadBE32(g.base(), view + kViewSize) == 2 && LoadBE32(g.base(), view + kViewSize + 4) == 1 &&
        LoadBE32(g.base(), view + kViewSize + 8) == 1);
  row = LoadBE32(g.base(), view + kViewSize + 12);
  CHECK(LoadBE64(g.base(), row) == kXuid && LoadBE32(g.base(), row + 8) == 1 && LoadBE64(g.base(), row + 16) == 700);

  const auto views = AnswerReadStats(g.base(), b.board, 2, 0x1800, 2, 0x1000);
  CHECK(views.size() == 2 && views[0].id == 1 && views[1].id == 2);
  CHECK(views[0].rows.size() == 1 && views[1].rows.size() == 1);
  CHECK(views[0].rows[0].xuid == kXuid && views[0].rows[0].rating == 0);
  CHECK(views[1].rows[0].xuid == kXuid && views[1].rows[0].rating == 700);
  // GetNumRowsInLB and GetTotalNumRowsInLB both read from this pair: a one-row self answer must agree on both,
  // so the script's own page-end clamp (mi_PageEnd = min(mi_NumRows, GetNumRowsInLB)) always lands on 1.
  for (const StatsView& answered : views) CHECK(answered.total_rows == answered.rows.size());
  for (const StatsView& answered : views) {
    for (const StatsRow& answered_row : answered.rows) CHECK(answered_row.xuid == kXuid);
  }

  // The unrelated XUID never gets a row, requested or not.
  CHECK(ServeReadStats(g.base(), b.board, 1, 0x1800, 2, 0x1000, 4096, 0x4000));
  view = LoadBE32(g.base(), 0x4004);
  CHECK(LoadBE32(g.base(), view + 8) == 0 && LoadBE32(g.base(), view + kViewSize + 8) == 0);

  // The game sizes the buffer as 8 + specs * (16 + xuids * 52) + xuids * columns * 28.
  const uint32_t game_size = 8 + 2 * (16 + 2 * 52) + 2 * 2 * 28;
  CHECK(StatsResultsSize(views) <= game_size);
  CHECK(ServeReadStats(g.base(), b.board, 2, 0x1800, 2, 0x1000, StatsResultsSize(views), 0x4000));
  CHECK(!ServeReadStats(g.base(), b.board, 2, 0x1800, 2, 0x1000, StatsResultsSize(views) - 1, 0x4000));
  CHECK(!ServeReadStats(g.base(), b.board, 2, 0x1800, 1, 0x1000, 4096, 0));
  CHECK(!ServeReadStats(g.base(), b.board, 2, 0x1800, 0, 0x1000, 4096, 0x4000));
  CHECK(!ServeReadStats(g.base(), b.board, 2, 0x1800, kMaxSpecs + 1, 0x1000, 4096, 0x4000));
  CHECK(!ServeReadStats(g.base(), b.board, 2, 0x1800, 1, 0, 4096, 0x4000));
}

void TestRepeatedPageChanges() {
  std::printf("page changes: every query is answered once and its handle retired\n");
  TempDir dir;
  Board b(dir);
  Guest g;
  g.Spec(0x1000, 1, {1});
  g.Spec(0x1100, 2, {1});
  g.Spec(0x1200, 3, {1});
  b.stats.Record(kXuid, 2, Stats(300));
  EnumeratorRegistry registry;
  uint32_t last_handle = 0;
  for (int page = 0; page < 60; ++page) {
    const uint32_t specs = 0x1000 + 0x100 * (page % 3);
    const uint32_t view_id = 1 + page % 3;
    uint32_t status;
    switch (page % 3) {
      case 0:
        status = CreateEnumerator(g.base(), registry, StatsQuery::Kind::kRanked, 1, 10, 1, specs, 0x3000, 0x3004);
        break;
      case 1:
        status = CreateEnumerator(g.base(), registry, StatsQuery::Kind::kByXuid, kXuid, 10, 1, specs, 0x3000, 0x3004);
        break;
      default:
        status = CreateFriendsEnumerator(g.base(), registry, 0x3000, 0x3004);
    }
    CHECK(status == kErrorSuccess);
    const uint32_t handle = LoadBE32(g.base(), 0x3004);
    CHECK(handle != last_handle);
    last_handle = handle;
    const auto query = registry.Take(handle);
    CHECK(query.has_value());
    if (!query) continue;
    const uint32_t served = ServeEnumeration(g.base(), b.board, *query, 0x4000, 4096, 0, 0x5000);
    CHECK(served == kErrorIoPending);
    const uint32_t expected = query->kind == StatsQuery::Kind::kFriends ? kErrorNoMoreFiles : kErrorSuccess;
    CHECK(LoadBE32(g.base(), 0x5000) == expected);
    if (query->kind != StatsQuery::Kind::kFriends) {
      const uint32_t view = LoadBE32(g.base(), 0x4004);
      CHECK(LoadBE32(g.base(), view) == view_id);
      CHECK(LoadBE32(g.base(), view + 8) == (view_id == 2 ? 1u : 0u));
    }
    CHECK(registry.pending() == 0);
    CHECK(!registry.Take(handle).has_value());
  }
}

void TestEnumeratorWrappersMatchTheShippedExecutable() {
  const std::filesystem::path image = std::filesystem::path(TMNT_SRC_DIR) / ".." / "game" / "analysis" / "default.image.bin";
  if (!std::filesystem::exists(image)) {
    std::printf("enumerator wrappers: image not available, skipped\n");
    return;
  }
  std::printf("enumerator wrappers: modes, pivot widths and the three callers in the shipped executable\n");
  const std::string bytes = ReadAll(image);
  constexpr uint32_t kImageBase = 0x82000000;
  auto word = [&](uint32_t at) {
    const size_t offset = at - kImageBase;
    if (offset + 4 > bytes.size()) return 0u;
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return std::byteswap(value);
  };
  auto branch_link = [](uint32_t at, uint32_t target) { return 0x48000001u | ((target - at) & 0x03FFFFFCu); };

  // XamUserCreateStatsEnumerator(title, mode, pivot, rows, spec count, specs, size*, handle*).
  CHECK(word(0x826EDFA0) == 0x7BC50020 && word(0x826EDFA4) == 0x38800001);  // rank: 32-bit pivot, mode 1
  CHECK(word(0x826EE000) == 0x7FC5F378 && word(0x826EE004) == 0x38800000);  // 64-bit pivot, mode 0
  CHECK(word(0x826EE060) == 0x7FC5F378 && word(0x826EE064) == 0x38800003);  // 64-bit pivot, mode 3
  // The only caller picks the wrapper from the query kind: 0 rank, 1 mode 3, 2 mode 0.
  CHECK(word(0x823CA2F0) == branch_link(0x823CA2F0, 0x826EDF58));
  CHECK(word(0x823CA310) == branch_link(0x823CA310, 0x826EE018));
  CHECK(word(0x823CA330) == branch_link(0x823CA330, 0x826EDFB8));

  auto calls_in = [&](uint32_t from, uint32_t to, uint32_t target) {
    int calls = 0;
    for (uint32_t at = from; at < to; at += 4) calls += word(at) == branch_link(at, target);
    return calls;
  };
  auto has_word = [&](uint32_t from, uint32_t to, uint32_t value) {
    for (uint32_t at = from; at < to; at += 4) {
      if (word(at) == value) return true;
    }
    return false;
  };
  // Kind 3 (friends' scores) reads with XUserReadStats: a size probe, then the read.
  CHECK(word(0x823CA358) == branch_link(0x823CA358, 0x826EDE80));
  CHECK(calls_in(0x823CA258, 0x823CA4D0, 0x826EDE80) == 2);
  CHECK(has_word(0x826EDE80, 0x826EDF58, 0x3C80000B) && has_word(0x826EDE80, 0x826EDF58, 0x60840021));  // 0xB0021

  // The friends resource: XFriendsCreateEnumerator (XMsgInProcessCall 0xFC, 0x58020), then XamEnumerate.
  CHECK(calls_in(0x823C36C8, 0x823C3820, 0x823C7548) == 1);
  CHECK(calls_in(0x823C36C8, 0x823C3820, 0x826EE470) == 1);
  CHECK(has_word(0x823C7548, 0x823C76A8, 0x386000FC) && has_word(0x823C7548, 0x823C76A8, 0x3C800005) &&
        has_word(0x823C7548, 0x823C76A8, 0x60848020));

  // The resource error reads the extended error at +0x18, and 0x80070012 becomes the script's "no more friends" (4).
  CHECK(word(0x826EE4A8) == 0x806B0018);
  CHECK(has_word(0x823C79D0, 0x823C7CF0, 0x3D608007) && has_word(0x823C79D0, 0x823C7CF0, 0x616A0012));
  CHECK(word(0x823C7ADC) == 0x38600004);
}

void TestResultsNeverRunPastTheBuffer() {
  std::printf("results: exactly the computed size is written\n");
  TempDir dir;
  Board b(dir);
  b.stats.Record(kXuid, 1, Stats(5, {{1, {kTypeInt64, 5}}, {2, {kTypeInt32, 2}}}));
  Guest g;
  g.Spec(0x1000, 1, {1, 2});
  const std::vector<StatsView> views = {b.board.Ranked(ReadSpecs(g.base(), 0x1000, 1)[0], 1, 100)};
  const uint32_t size = StatsResultsSize(views);
  CHECK(size == kResultsHeaderSize + kViewSize + kRowSize + 2 * kColumnSize);
  CHECK(WriteStatsResults(g.base(), 0x6000, size, views));
  CHECK(g.base()[0x6000 + size] == 0xCD && g.base()[0x6000 - 1] == 0xCD);
  CHECK(!WriteStatsResults(g.base(), 0x6000, size - 1, views));
  CHECK(!WriteStatsResults(g.base(), 0, size, views));
}

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::string Between(const std::string& text, const char* from, const char* to) {
  const size_t start = text.find(from);
  const size_t end = text.find(to, start == std::string::npos ? 0 : start);
  return start == std::string::npos || end == std::string::npos ? std::string() : text.substr(start, end - start);
}

void TestRegisteredViewScore() {
  std::printf("registry: the game's own score is read before any session write\n");
  Guest g;
  g.Property(0x1000, 0x10000001, kTypeInt64, 4200);
  g.Property(0x1018, 0x10000007, kTypeInt32, 9);
  StoreBE32(g.base(), 0x3000, 3);
  StoreBE32(g.base(), 0x3008, 0x4000);
  const uint32_t second = 0x4000 + kRegisteredViewSize;
  StoreBE32(g.base(), second, 7);
  StoreBE32(g.base(), second + 8, 2);
  StoreBE32(g.base(), second + 20, 0x1000);

  const auto write = ParseRegisteredView(g.base(), 0x3000, 1);
  CHECK(write.has_value());
  CHECK(write->view == 7 && write->stats.rating == 4200);
  CHECK(write->stats.columns.at(7) == (StatColumn{kTypeInt32, 9}));

  StoreBE32(g.base(), 0x5000, 1);
  StoreBE32(g.base(), 0x5004, 9);
  const auto several = ParseRegisteredViews(g.base(), 0x3000, 0x5000, 2);
  CHECK(several.size() == 1 && several[0].view == 7);
  CHECK(ParseRegisteredViews(g.base(), 0x3000, 0, 1).empty());
  CHECK(ParseRegisteredViews(g.base(), 0x3000, 0x5000, 17).empty());

  CHECK(!ParseRegisteredView(g.base(), 0x3000, 3).has_value());
  CHECK(!ParseRegisteredView(g.base(), 0x3000, 0).has_value());
  CHECK(!ParseRegisteredView(g.base(), 0, 1).has_value());

  TempDir dir;
  const auto file = dir.path / "stats.toml";
  {
    LocalStats stats(file);
    stats.Load();
    CHECK(stats.Record(kXuid, write->view, write->stats) == RecordResult::kSaved);
    g.Property(0x1000, 0x10000001, kTypeInt64, 100);
    const auto worse = ParseRegisteredView(g.base(), 0x3000, 1);
    CHECK(stats.Record(kXuid, worse->view, worse->stats) == RecordResult::kUnchanged);
  }
  LocalStats reloaded(file);
  CHECK(reloaded.Load() == LoadResult::kLoaded);
  CHECK(reloaded.Get(kXuid, 7) && reloaded.Get(kXuid, 7)->rating == 4200);
}

void TestLiveScope() {
  std::printf("live scope: false outside, true inside, restored after\n");
  CHECK(!LiveScopeActive());
  {
    LiveScope scope("outer");
    CHECK(LiveScopeActive());
    CHECK(std::string(t_live_scope_name) == "outer");
    {
      LiveScope inner("inner");
      CHECK(LiveScopeActive() && std::string(t_live_scope_name) == "inner");
    }
    CHECK(LiveScopeActive() && std::string(t_live_scope_name) == "outer");
    {
      LiveScopeSuspended offline;
      CHECK(!LiveScopeActive());
      {
        LiveScope nested("nested");
        CHECK(LiveScopeActive());
      }
      CHECK(!LiveScopeActive());
    }
    CHECK(LiveScopeActive());
  }
  CHECK(!LiveScopeActive() && t_live_scope_name == nullptr);

  bool other_thread_live = true;
  {
    LiveScope scope("main thread");
    std::thread([&] { other_thread_live = LiveScopeActive(); }).join();
  }
  CHECK(!other_thread_live);

  try {
    LiveScope scope("unwinding");
    throw 1;
  } catch (int) {
  }
  CHECK(!LiveScopeActive());
}

void TestLocalProfileAccepted() {
  std::printf("local profile: accepted in every local scope, offline outside, no sign-in state needed\n");
  for (int32_t controller = 0; controller < kLocalControllers; ++controller) {
    CHECK(!LocalProfileAccepted(controller));
  }
  const char* scopes[] = {"Leaderboard_SwitchPage", "PLT_CheckPlayerSignIn", "StartMenuWorld_CheckSignIn",
                          LocalOnlineEventName(kFireEventProceedSignal, kProceedSourceLeaderboard),
                          LocalOnlineEventName(kFireEventSpInit, 0),
                          LocalOnlineEventName(kFireEventProceedSignal, kProceedSourceMpLocal)};
  for (const char* name : scopes) {
    CHECK(name != nullptr);
    LiveScope scope(name);
    for (int32_t controller = 0; controller < kLocalControllers; ++controller) {
      CHECK(LocalProfileAccepted(controller));
    }
    CHECK(!LocalProfileAccepted(-1) && !LocalProfileAccepted(kLocalControllers));
  }
  for (int32_t controller = 0; controller < kLocalControllers; ++controller) {
    CHECK(!LocalProfileAccepted(controller));
  }

  {
    LiveScope outer("PROCEED_MP_LOCAL");
    {
      LiveScope inner("Leaderboard_SwitchPage");
      CHECK(LocalProfileAccepted(1));
    }
    CHECK(LocalProfileAccepted(1) && std::string(t_live_scope_name) == "PROCEED_MP_LOCAL");
    {
      LiveScopeSuspended session;
      CHECK(!LocalProfileAccepted(0) && !LocalProfileAccepted(1));
    }
    CHECK(LocalProfileAccepted(0));
  }
  CHECK(!LocalProfileAccepted(0) && t_live_scope_name == nullptr);
}

void TestForcedImport() {
  std::printf("forced import: only the named index, only inside the scope\n");
  CHECK(!ImportForced(kImportPlayerSignedIntoConsole));
  CHECK(!ImportForced(0));
  {
    ForcedImport forced(kImportPlayerSignedIntoConsole);
    CHECK(ImportForced(kImportPlayerSignedIntoConsole));
    CHECK(!ImportForced(1876) && !ImportForced(1954));
  }
  CHECK(!ImportForced(kImportPlayerSignedIntoConsole));
}

void TestLocalOnlineEvents() {
  std::printf("local online events: leaderboard, local multiplayer and single player only\n");
  CHECK(std::string(LocalOnlineEventName(kFireEventProceedSignal, kProceedSourceLeaderboard)) == "PROCEED_LEADERBRD");
  CHECK(std::string(LocalOnlineEventName(kFireEventProceedSignal, kProceedSourceMpLocal)) == "PROCEED_MP_LOCAL");
  CHECK(std::string(LocalOnlineEventName(kFireEventSpInit, 0)) == "REC_SP_INIT");
  for (uint32_t source = 0; source < 16; ++source) {
    if (source == kProceedSourceLeaderboard || source == kProceedSourceMpLocal) continue;
    CHECK(LocalOnlineEventName(kFireEventProceedSignal, source) == nullptr);
  }
  constexpr uint32_t kRecLoadMp = 198;
  constexpr uint32_t kRecLoadPl = 199;
  CHECK(LocalOnlineEventName(kRecLoadMp, kProceedSourceLeaderboard) == nullptr);
  CHECK(LocalOnlineEventName(kRecLoadPl, kProceedSourceMpLocal) == nullptr);
  CHECK(LocalOnlineEventName(0, 0) == nullptr);
  CHECK(LocalOnlineEventName(kProceedSourceLeaderboard, kProceedSourceLeaderboard) == nullptr);
  CHECK(kFireEventSourceOffset == 8);
}

void TestLocalPlayerJoinNotification() {
  std::printf("local join: only LOC_NOTF_LEADERBRD_NOT_XLA is dropped, only while a local player registers\n");
  CHECK(kNotificationLeaderboardNotXla == 16);
  CHECK(!DropsNotification(kNotificationLeaderboardNotXla));
  {
    LocalPlayerJoin joining;
    CHECK(DropsNotification(kNotificationLeaderboardNotXla));
    for (uint32_t id = 0; id < 64; ++id) {
      if (id != kNotificationLeaderboardNotXla) CHECK(!DropsNotification(id));
    }
    {
      LocalPlayerJoin nested;
      CHECK(DropsNotification(kNotificationLeaderboardNotXla));
    }
    CHECK(DropsNotification(kNotificationLeaderboardNotXla));
    CHECK(!LiveScopeActive());
  }
  CHECK(!DropsNotification(kNotificationLeaderboardNotXla));
  bool other_thread_drops = true;
  {
    LocalPlayerJoin joining;
    std::thread([&] { other_thread_drops = DropsNotification(kNotificationLeaderboardNotXla); }).join();
  }
  CHECK(!other_thread_drops);

  const std::string module = ReadSource("live_hooks_tmnt_r.cpp");
  const std::string join = Between(module, "REX_HOOK_RAW(sub_876B6D08)", "REX_EXTERN(__imp__sub_875BA888)");
  CHECK(Contains(join, "LocalPlayerJoin joining;") && Contains(join, "__imp__sub_876B6D08(ctx, base);"));
  CHECK(!Contains(join, "LiveScope") && !Contains(join, "ForcedImport"));
  const std::string drop = Between(module, "REX_HOOK_RAW(sub_875BA888)", "REX_EXTERN(__imp__sub_875A6770)");
  CHECK(Contains(drop, "DropsNotification(ctx.r4.u32)") && Contains(drop, "__imp__sub_875BA888(ctx, base);"));
  CHECK(!Contains(module, "PlatformSession") && !Contains(module, "sub_876AE008) = "));
}

void TestHooksAreDirect() {
  std::printf("hooks: the TMNT_r functions are hooked directly, no call-site discrimination\n");
  const std::string module = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(!module.empty());
  const char* hooked[] = {"REX_HOOK_RAW(sub_876AE008)", "REX_HOOK_RAW(sub_8759F858)", "REX_HOOK_RAW(sub_875A11B0)",
                          "REX_HOOK_RAW(sub_876AE720)",  "REX_HOOK_RAW(sub_876B75C0)", "REX_HOOK_RAW(sub_875BA998)",
                          "REX_HOOK_RAW(sub_875A6770)",  "REX_HOOK_RAW(sub_87599B80)", "REX_HOOK_RAW(sub_875BA650)",
                          "REX_HOOK_RAW(sub_87520018)", "REX_HOOK_RAW(sub_876B6D08)",
                          "REX_HOOK_RAW(sub_875BA888)"};
  for (const char* hook : hooked) CHECK(Contains(module, hook));

  // Leaderboard_SendDataToFire: a postcondition repair. The gate is the single line checked below; prove its
  // boundary semantics directly rather than just that the line exists.
  const std::string send_data = Between(module, "REX_HOOK_RAW(sub_875A11B0)", "REX_EXTERN(__imp__sub_876AE720)");
  CHECK(Contains(send_data, "LoadBE32(base, ctx.r3.u32 + 20)"));
  CHECK(Contains(send_data, "LoadBE32(base, vars + 8)") && Contains(send_data, "LoadBE32(base, vars + 12)"));
  CHECK(Contains(send_data, "__imp__sub_875A11B0(ctx, base);"));
  const char* const kGateLine = "if (page_start < page_end) return;";
  CHECK(Contains(send_data, kGateLine));
  CHECK(Contains(send_data, "CompleteLeaderboardTrack(ctx, base, lr);"));
  CHECK(Contains(send_data, "[Leaderboard] forced completed leaderboard read start={} end={}"));

  const auto gate_returns_early = [](uint32_t page_start, uint32_t page_end) { return page_start < page_end; };
  CHECK(!gate_returns_early(1, 1));  // one-row page, fully sent: the repair fires
  CHECK(!gate_returns_early(0, 0));  // zero-row page, never had a row to send: the repair fires immediately
  CHECK(gate_returns_early(0, 1));   // one row still pending: the repair must not fire
  // Once the repair's own SCR_TrackChange takes effect, the VM stops dispatching to this track, so this hook
  // body (and the gate above) is never evaluated again for that completed query: no separate guard is needed.

  const std::string repair = Between(module, "void CompleteLeaderboardTrack", "REX_EXTERN(__imp__sub_875A11B0)");
  CHECK(Contains(repair, "resolve_import(1855)"));
  CHECK(Contains(repair, "resolve_import(11)"));
  CHECK(Contains(repair, "sub_875B51D0(ctx, base);"));
  CHECK(Contains(repair, "resolve_import(1342)"));
  CHECK(Contains(repair, "kFireUiHandleSlot") && Contains(repair, "kLeaderboardMainNameSlot"));
  CHECK(!Contains(module, "REX_HOOK_RAW(sub_875B51D0)"));
  CHECK(!Contains(module, "Sleep(") && !Contains(module, "std::this_thread") && !Contains(module, "chrono"));
  CHECK(!Contains(module, "GetNumRowsInLB") && !Contains(module, "GetRowPlayerName"));

  const std::string helper = Between(module, "REX_HOOK_RAW(sub_876AE008)", "REX_HOOK_RAW(sub_8759F858)");
  CHECK(Contains(helper, "const int32_t controller = ctx.r5.s32;"));
  CHECK(Contains(helper, "if (ctx.r3.u32 || !LocalProfileAccepted(controller)) return;"));
  CHECK(Contains(helper, "ctx.r3.u64 = 1;"));
  CHECK(!Contains(module, "sub_876ADF58"));
  CHECK(!Contains(module, "SigninState") && !Contains(module, "signin_state"));
  CHECK(Contains(module, "[Live] local profile accepted in {}"));

  CHECK(Contains(Between(module, "REX_HOOK_RAW(sub_8759F858)", "REX_EXTERN(__imp__sub_876AE720)"), "LiveScope scope("));
  CHECK(Contains(Between(module, "REX_HOOK_RAW(sub_876AE720)", "REX_EXTERN(__imp__sub_876B75C0)"), "LiveScope scope("));
  CHECK(Contains(Between(module, "REX_HOOK_RAW(sub_876B75C0)", "REX_EXTERN(__imp__sub_875BA998)"), "LiveScope scope("));
  const std::string events = Between(module, "REX_HOOK_RAW(sub_875BA998)", "REX_EXTERN(__imp__sub_875A6770)");
  CHECK(Contains(events, "LocalOnlineEventName(event, track)"));
  CHECK(Contains(events, "LiveScope scope(name);"));
  CHECK(Contains(events, "LiveScopeSuspended offline;"));

  CHECK(!Contains(module, "FirstCall") && !Contains(module, "DEBUG_SAULO"));

  CHECK(!Contains(module, "ctx.lr =="));
  CHECK(!Contains(module, "ctx.lr !="));
  CHECK(!Contains(module, "chrono"));
  CHECK(!Contains(module, "Token"));
  CHECK(!Contains(module, "XSessionCreate") && !Contains(module, "XSessionJoin") && !Contains(module, "XSessionStart"));
  CHECK(!Contains(module, "REC_LOAD") && !Contains(module, "SessionManager"));
  CHECK(!Contains(module, "__imp__XamUserGetSigninState"));

  const std::string score = Between(module, "REX_HOOK_RAW(sub_87599B80)", "REX_EXTERN(__imp__sub_875BA650)");
  CHECK(Contains(score, "if (t_stats_requested) return;"));
  CHECK(!Contains(module, "REX_HOOK_RAW(sub_822FFC88)"));

  const std::string label = Between(module, "REX_HOOK_RAW(sub_875BA650)", "REX_HOOK_RAW(sub_87520018)");
  CHECK(Contains(label, "HasLocalDisplayName(ctx.r4.u32)"));
  CHECK(Contains(label, "ForcedImport signed_in(kImportPlayerSignedIntoConsole);"));
  CHECK(Contains(module, "local_player_"));

  const std::string live = ReadSource("live_fix.cpp");
  CHECK(!live.empty());
  CHECK(!Contains(live, "sub_822FD0F0") && !Contains(live, "sub_822FD0C8"));
  CHECK(!Contains(live, "live_sites.h") && !Contains(live, "FindLocalOnlineSite"));
  CHECK(!Contains(live, "kConsoleSignInCaller") && !Contains(live, "ctx.lr"));
  CHECK(!Contains(live, "XSessionCreate") && !Contains(live, "XSessionJoin"));
  CHECK(!Contains(live, "FireCommunicator") && !Contains(live, "LiveScope"));
  CHECK(Contains(live, "REX_HOOK_RAW(sub_822FDF78)"));
  CHECK(Contains(live, "ParseRegisteredViews(base, LoadBE32(base, kLeaderboardRegistryGlobal), ctx.r5.u32, ctx.r4.u32)"));
  CHECK(Contains(live, "write.stats.rating > 0"));
  CHECK(Contains(live, "xuid != services.board.user().xuid"));
  CHECK(Contains(live, "REX_HOOK_RAW(sub_8243CCB8)"));
  CHECK(Contains(live, "REX_HOOK_RAW(sub_826EE470)"));
  CHECK(Contains(live, "REX_HOOK_RAW(sub_823C7548)"));
  CHECK(Contains(live, "CreateFriendsEnumerator(base, services->enumerators, ctx.r6.u32, ctx.r7.u32)"));
  CHECK(Contains(live, "REX_HOOK_RAW(sub_826EDE80)"));
  CHECK(Contains(Between(live, "REX_HOOK_RAW(sub_826EDE80)", "\n}\n"), "__imp__sub_826EDE80(ctx, base);"));
  CHECK(!Contains(live, "LogQuery") && !Contains(live, "DEBUG_SAULO"));
  CHECK(!Contains(live, "GetNumRowsInLB") && !Contains(live, "GetRowPlayerName") && !Contains(live, "SendData"));
  CHECK(!Contains(live, "DoWaitStop") && !Contains(live, "std::this_thread") && !Contains(live, "chrono"));
  CHECK(!Contains(live, "kByRating"));
  CHECK(!std::filesystem::exists(std::filesystem::path(TMNT_SRC_DIR) / "live_sites.h"));

  std::ifstream cmake(std::filesystem::path(TMNT_SRC_DIR) / ".." / "CMakeLists.txt");
  std::ostringstream cmake_text;
  cmake_text << cmake.rdbuf();
  const std::string tmnt_r_sources =
      Between(cmake_text.str(), "target_sources(tmnt_turtles_in_time_reshelled_TMNT_r PRIVATE", ")");
  CHECK(Contains(tmnt_r_sources, "src/live_hooks_tmnt_r.cpp"));

  const std::string achievements = ReadSource("achievement_menu.cpp");
  CHECK(Contains(achievements, "REX_HOOK_RAW(sub_826EDB50)"));
  CHECK(Contains(achievements, "SetAchievementMenuOpen"));

  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Contains(app, ".user = {profile->xuid(), rex::cvar::GetFlagByName(tmnt::kPlayerNames[0].sdk_key)}"));
  CHECK(!Contains(app, ".user = {profile->xuid(), profile->name()}"));
  CHECK(!Contains(app, "\"saulo\"") && !Contains(app, "\"User\""));

  const std::string read_stats = Between(live, "REX_HOOK_RAW(sub_826EDE80)", "REX_EXTERN(__imp__sub_822FDF78)");
  CHECK(Contains(read_stats, "CompleteOverlapped(base, overlapped, served ? kErrorSuccess : kErrorInsufficientBuffer, served ? 1 : 0)"));
}

void TestHooksMatchTheShippedModule() {
  const std::filesystem::path image = std::filesystem::path(TMNT_SRC_DIR) / ".." / "game" / "analysis" / "tmnt_r.image.bin";
  if (!std::filesystem::exists(image)) {
    std::printf("shipped module: image not available, skipped\n");
    return;
  }
  std::printf("shipped module: hooked entry points, event table and score gate in the TMNT_r image\n");
  const std::string bytes = ReadAll(image);
  constexpr uint32_t kModuleBase = 0x87500000;
  auto word = [&](uint32_t at) {
    const size_t offset = at - kModuleBase;
    if (offset + 4 > bytes.size()) return 0u;
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return std::byteswap(value);
  };
  auto branch_link = [](uint32_t at, uint32_t target) { return 0x48000001u | ((target - at) & 0x03FFFFFCu); };

  constexpr uint32_t kMflrR12 = 0x7D8802A6;
  const uint32_t entries[] = {0x876AE008, 0x8759F858, 0x875A11B0, 0x876AE720, 0x876B75C0,
                              0x875BA998, 0x875A6770, 0x87599B80, 0x875BA650, 0x876B6D08, 0x875BA888};
  for (uint32_t entry : entries) CHECK(word(entry) == kMflrR12);
  CHECK(word(0x87520018) == 0x3D60876E);  // lis r11,0x876E: the import table lookup

  // Leaderboard_SendDataToFire: this+20+8 is mi_PageStart, this+20+12 is mi_PageEnd. The success path increments
  // mi_PageStart at 0x875A1664/0x875A166C, then both paths compare it against mi_PageEnd at 0x875A1784/0x875A1788.
  CHECK(word(0x875A1660) == 0x817F0014 && word(0x875A1664) == 0x814B0008);  // lwz r11,20(r31); lwz r10,8(r11)
  CHECK(word(0x875A1668) == 0x392A0001 && word(0x875A166C) == 0x912B0008);  // addi r9,r10,1; stw r9,8(r11)
  CHECK(word(0x875A1780) == 0x817F0014 && word(0x875A1784) == 0x814B0008);  // lwz r11,20(r31); lwz r10,8(r11)
  CHECK(word(0x875A1788) == 0x812B000C);                                   // lwz r9,12(r11)
  CHECK(word(0x875A178C) == 0x7F0A4800 && word(0x875A1790) == 0x41980068);  // cmpw cr6,r10,r9; blt cr6,+0x68

  // FireCommunicator_OnReceiveEvent: var in r4 -> r30; the event switch is offset by 121.
  constexpr uint32_t kEventTable = 0x875BAC18;
  constexpr uint32_t kFirstEvent = 121;
  CHECK(word(0x875BA9B0) == 0x7C9E2378);
  CHECK(word(kEventTable + 4 * (kFireEventSpInit - kFirstEvent)) == 0x875C0638);
  CHECK(word(kEventTable + 4 * (kFireEventProceedSignal - kFirstEvent)) == 0x875BE4AC);
  CHECK(word(kEventTable + 4 * (198 - kFirstEvent)) == 0x875BB240);
  CHECK(word(kEventTable + 4 * (199 - kFirstEvent)) == 0x875BADC4);
  CHECK(word(0x875BE4B0) == 0x83BE0004);                      // joy_id = var.i32_2
  CHECK(word(0x875BE4B4) == 0x839E0000 + kFireEventSourceOffset);  // source = var.i32_3

  // InGameHUD_GoShowSummary: the single 1954 lookup gates the request, then the 5 / 1855 / 11 lookups build its target.
  CHECK(word(0x8759A3F4) == 0x386007A2);
  CHECK(word(0x8759A404) == 0x2F030000);
  CHECK(word(0x8759A408) == 0x419A005C);
  CHECK(word(0x8759A40C) == 0x38600005);
  CHECK(word(0x8759A41C) == 0x39631C30);
  CHECK(word(0x8759A420) == 0x3860073F);
  CHECK(word(0x8759A434) == 0x38800000 && word(0x8759A438) == 0x38A00000);
  CHECK(word(0x8759A448) == 0x3860000B);
  CHECK(word(0x8759A460) == branch_link(0x8759A460, 0x875A6770));

  // SendCharSelGamerTag: joy_id (r4 -> r28) is what the console sign-in lookup (1875) receives.
  CHECK(word(0x875BA668) == 0x7C9C2378);
  CHECK(word(0x875BA75C) == 0x38600753);
  CHECK(word(0x875BA768) == 0x7F83E378);

  // The stub that stands in for the forced lookup returns true and nothing refers to it.
  constexpr uint32_t kReturnsTrue = 0x8761B470;
  CHECK(word(kReturnsTrue) == 0x38600001 && word(kReturnsTrue + 4) == 0x4E800020);
  int stub_refs = 0;
  int request_refs = 0;
  for (uint32_t at = kModuleBase; at + 4 <= kModuleBase + bytes.size(); at += 4) {
    const uint32_t w = word(at);
    if (w == kReturnsTrue) ++stub_refs;
    if (w == branch_link(at, kReturnsTrue) || w == (branch_link(at, kReturnsTrue) & ~1u)) ++stub_refs;
    if (w == branch_link(at, 0x875A6770) || w == (branch_link(at, 0x875A6770) & ~1u)) ++request_refs;
  }
  CHECK(stub_refs == 0);
  CHECK(request_refs == 1);
}

}  // namespace

int main() {
  TestCleanFileAndFirstValue();
  TestRestartKeepsValues();
  TestBestScoreAndIndependentViews();
  TestRejectedSubmissions();
  TestMalformedFiles();
  TestAtomicReplacement();
  TestPathSafety();
  TestParseViewWrites();
  TestEnumeratorCreation();
  TestRegistryRetiresAndBounds();
  TestEnumerationAnswers();
  TestEnumerationBufferAndOverlapped();
  TestFriendsHaveNobody();
  TestReadStats();
  TestRepeatedPageChanges();
  TestEnumeratorWrappersMatchTheShippedExecutable();
  TestResultsNeverRunPastTheBuffer();
  TestRegisteredViewScore();
  TestLiveScope();
  TestLocalProfileAccepted();
  TestForcedImport();
  TestLocalOnlineEvents();
  TestLocalPlayerJoinNotification();
  TestHooksAreDirect();
  TestHooksMatchTheShippedModule();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
