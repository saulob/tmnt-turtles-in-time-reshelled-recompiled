// Menu movie edits: the edit rules, the table, the removed Gamer Card host code and, when the game data is present, the
// real movies: their edited bytecode still walks, and the rows, navigation and footers come out as intended.
// Run: tmnt_fire_movie_patch_tests

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/fire_movie_patch.cpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

using namespace tmnt;
namespace fs = std::filesystem;
using Rows = std::vector<std::string>;

std::string ReadFile(const std::string& relative) {
  std::ifstream in(fs::path(TMNT_ROOT_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestEditRules() {
  std::printf("edits: applied together or not at all\n");
  const MovieEdit edits[] = {{1, "0203", "0a0b"}, {4, "05", "0f"}};
  std::vector<uint8_t> movie = {1, 2, 3, 4, 5};
  CHECK(ApplyMovieEdits(movie.data(), movie.size(), edits));
  CHECK((movie == std::vector<uint8_t>{1, 10, 11, 4, 15}));
  CHECK(!ApplyMovieEdits(movie.data(), movie.size(), edits));

  std::vector<uint8_t> altered = {1, 2, 3, 4, 6};
  CHECK(!ApplyMovieEdits(altered.data(), altered.size(), edits));
  CHECK((altered == std::vector<uint8_t>{1, 2, 3, 4, 6}));

  std::vector<uint8_t> small = {1, 2};
  CHECK(!ApplyMovieEdits(small.data(), small.size(), edits));
  CHECK(!ApplyMovieEdits(nullptr, 5, edits));
  const MovieEdit uneven[] = {{0, "01", "0a0b"}};
  CHECK(!ApplyMovieEdits(movie.data(), movie.size(), uneven));
  const MovieEdit odd[] = {{0, "1", "2"}};
  CHECK(!ApplyMovieEdits(movie.data(), movie.size(), odd));
}

void TestTable() {
  std::printf("table: every edit stays inside its movie, and a movie inside its bundle\n");
  std::set<std::string_view> names;
  for (const MenuMovie& movie : MenuMovies()) {
    names.insert(movie.name);
    CHECK(IsMenuBundle(movie.bundle_bytes));
    CHECK(movie.offset + movie.bytes <= movie.bundle_bytes);
    size_t end = 0;
    for (const MovieEdit& edit : movie.edits) {
      CHECK(edit.before.size() == edit.after.size());
      CHECK(edit.before.size() % 2 == 0 && !edit.before.empty());
      CHECK(edit.offset >= end);
      end = edit.offset + edit.before.size() / 2;
      CHECK(end <= movie.bytes);
    }
  }
  for (const char* name : {"online", "menu_main", "pause", "lobby", "matchmaking", "summary", "game_over"}) {
    CHECK(names.count(name) == 1);
  }
  CHECK(!IsMenuBundle(115401));
  CHECK(!IsMenuBundle(0));

  std::printf("table: a bundle whose movies are unknown stays as it is\n");
  std::vector<uint8_t> bundle(15331229, 0x55);
  const BundlePatchResult result = PatchMenuBundle(bundle.data(), bundle.size());
  CHECK(result.patched == 0 && result.failed == 3);
  CHECK(std::all_of(bundle.begin(), bundle.end(), [](uint8_t b) { return b == 0x55; }));
  const BundlePatchResult none = PatchMenuBundle(nullptr, bundle.size());
  CHECK(none.patched == 0 && none.failed == 0);
}

void TestGamerCardIsGone() {
  std::printf("source: no Gamer Card bridge, overlay, hook, bind, build entry or test is left\n");
  for (const char* gone : {"src/gamer_card.cpp", "src/gamer_card.h", "src/gamer_card_overlay.cpp", "src/gamer_card_overlay.h",
                           "src/gamer_card_rules.h", "tests/gamer_card_tests.cpp"}) {
    CHECK(!fs::exists(fs::path(TMNT_ROOT_DIR) / gone));
  }
  CHECK(!Has(ReadFile("CMakeLists.txt"), "gamer_card"));
  for (const auto& entry : fs::recursive_directory_iterator(fs::path(TMNT_ROOT_DIR) / "src")) {
    const std::string extension = entry.path().extension().string();
    if (extension != ".cpp" && extension != ".h") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::stringstream source;
    source << in.rdbuf();
    const std::string text = source.str();
    CHECK(!Has(text, "GamerCard"));
    CHECK(!Has(text, "gamer_card"));
    CHECK(!Has(text, "sub_826EDB48"));
    CHECK(!Has(text, "sub_8757E9E0"));
  }

  std::printf("source: Achievements, Invite Friends and the hook stay\n");
  const std::string app = ReadFile("src/tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::InstallAchievementMenu"));
  CHECK(Has(app, "new tmnt::AchievementsOverlay"));
  CHECK(Has(ReadFile("src/keybinds.h"), "ImGuiKey_F7"));
  CHECK(Has(ReadFile("src/player_match_invite_hook.cpp"), "TMNT_INVITE_HOOK(826EDB40"));
  const std::string hook = ReadFile("src/fire_menu_hook.cpp");
  CHECK(Has(hook, "REX_HOOK_RAW(sub_824EC4C8)"));
  CHECK(Has(hook, "tmnt::IsMenuBundle(size)"));
  CHECK(Has(hook, "tmnt::PatchMenuBundle("));
  CHECK(Has(ReadFile("CMakeLists.txt"), "src/fire_menu_hook.cpp"));
}

// ---- The game's own data ---------------------------------------------------------------------------------------------

std::vector<uint8_t> DecompressLzo1x(const uint8_t* in, size_t reserve) {
  std::vector<uint8_t> out;
  out.reserve(reserve);
  size_t ip = 0;
  const auto extended = [&](size_t value) {
    while (in[ip] == 0) {
      value += 255;
      ++ip;
    }
    return value + in[ip++];
  };
  size_t state = 0;
  if (in[0] > 17) {
    const size_t count = in[0] - 17;
    out.insert(out.end(), in + 1, in + 1 + count);
    ip = 1 + count;
    state = std::min<size_t>(count, 4);
  }
  for (;;) {
    const size_t t = in[ip++];
    size_t distance, length, trail;
    if (t >= 64) {
      distance = ((t >> 2) & 7) + (size_t(in[ip++]) << 3) + 1;
      length = (t >> 5) + 1;
      trail = t & 3;
    } else if (t >= 32) {
      length = ((t & 31) ? (t & 31) : extended(31)) + 2;
      const size_t d = in[ip] | size_t(in[ip + 1]) << 8;
      ip += 2;
      distance = (d >> 2) + 1;
      trail = d & 3;
    } else if (t >= 16) {
      length = ((t & 7) ? (t & 7) : extended(7)) + 2;
      const size_t d = in[ip] | size_t(in[ip + 1]) << 8;
      ip += 2;
      distance = ((t & 8) << 11) + (d >> 2);
      if (!distance) return out;
      distance += 0x4000;
      trail = d & 3;
    } else if (!state) {
      const size_t count = (t ? t : extended(15)) + 3;
      out.insert(out.end(), in + ip, in + ip + count);
      ip += count;
      state = 4;
      continue;
    } else {
      distance = (t >> 2) + (size_t(in[ip++]) << 2) + (state < 4 ? 1 : 2049);
      length = state < 4 ? 2 : 3;
      trail = t & 3;
    }
    const size_t from = out.size() - distance;
    for (size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    out.insert(out.end(), in + ip, in + ip + trail);
    ip += trail;
    state = trail;
  }
}

template <typename T>
T Read(const uint8_t* data, size_t offset) {
  T value;
  std::memcpy(&value, data + offset, sizeof value);
  return value;
}

struct BitReader {
  const uint8_t* data;
  size_t bit;
  uint32_t Unsigned(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i, ++bit) value = value << 1 | ((data[bit / 8] >> (7 - bit % 8)) & 1);
    return value;
  }
  int32_t Signed(int count) {
    const uint32_t value = Unsigned(count);
    return count && value >> (count - 1) ? int32_t(value) - (1 << count) : int32_t(value);
  }
  size_t Byte() const { return (bit + 7) / 8; }
};

struct Instance {
  int x = 0;
  int y = 0;
  int depth = 0;
};

struct Action {
  uint8_t op = 0;
  size_t body = 0;
  size_t length = 0;
};

constexpr uint8_t kGetVariable = 0x1C, kInitArray = 0x42, kPush = 0x96, kJump = 0x99, kIf = 0x9D, kConstantPool = 0x88,
                  kDefineFunction = 0x9B, kDefineFunction2 = 0x8E;

// One value of a Push: a pool string, an int, a double or a bool.
struct Value {
  enum Kind { kOther, kString, kNumber, kBool } kind = kOther;
  std::string text;
  double number = 0;
};

class Movie {
 public:
  Movie(const uint8_t* data, size_t size) : data_(data) {
    BitReader bits{data, 8 * 8};
    const int rect = bits.Unsigned(5);
    bits.Unsigned(rect), bits.Unsigned(rect), bits.Unsigned(rect), bits.Unsigned(rect);
    WalkTags(bits.Byte() + 4, size, true);
  }

  bool Valid() const { return valid_ && !actions_.empty(); }
  const std::map<std::string, Instance>& Instances() const { return instances_; }
  bool Reached(size_t offset) const { return actions_.count(offset) != 0; }

  // `name._visible = false` on a path the movie can take.
  bool Hides(const std::string& name) const {
    for (const auto& [offset, action] : actions_) {
      const std::vector<Value> first = Values(action);
      if (first.size() != 1 || first[0].kind != Value::kString || first[0].text != name) continue;
      const size_t next = offset + 3 + action.length;
      const auto get = actions_.find(next);
      if (get == actions_.end() || get->second.op != kGetVariable) continue;
      const auto push = actions_.find(next + 1);
      if (push == actions_.end()) continue;
      const std::vector<Value> second = Values(push->second);
      if (second.size() == 2 && second[0].text == "_visible" && second[1].kind == Value::kBool && second[1].number == 0) return true;
    }
    return false;
  }

  std::set<int> LastTabs() const {
    std::set<int> tabs;
    for (const auto& [offset, action] : actions_) {
      const std::vector<Value> values = Values(action);
      if (values.size() == 2 && values[0].text == "LAST_TAB") tabs.insert(int(values[1].number));
    }
    return tabs;
  }

  // `is_unlocked = [...]`; the single array the movie builds that way.
  std::vector<int> UnlockedTable() const {
    for (const auto& [offset, action] : actions_) {
      if (action.op != kInitArray) continue;
      auto push = actions_.lower_bound(offset);
      if (push == actions_.begin()) continue;
      const std::vector<Value> values = Values(std::prev(push)->second);
      if (values.empty()) continue;
      std::vector<int> table;
      for (size_t i = values.size() - 1; i-- > 0;) table.push_back(int(values[i].number));
      return table;
    }
    return {};
  }

 private:
  void WalkTags(size_t at, size_t end, bool root) {
    int frame = 0;
    while (at < end) {
      const uint16_t header = Read<uint16_t>(data_, at);
      const int code = header >> 6;
      size_t length = header & 63;
      at += 2;
      if (length == 63) {
        length = Read<uint32_t>(data_, at);
        at += 4;
      }
      if (at + length > end) {
        valid_ = false;
        return;
      }
      if (code == 12 && root && frame == 0 && actions_.empty()) WalkActions(at, at + length);
      if (code == 26 && root && frame == 0) ReadPlacement(at);
      if (code == 1) ++frame;
      if (code == 0) return;
      at += length;
    }
  }

  void ReadPlacement(size_t at) {
    const uint8_t flags = data_[at];
    if ((flags & 0x24) != 0x24) return;
    Instance instance;
    instance.depth = Read<uint16_t>(data_, at + 1);
    BitReader bits{data_, (at + 3 + (flags & 2 ? 2 : 0)) * 8};
    if (bits.Unsigned(1)) {
      const int count = bits.Unsigned(5);
      bits.Unsigned(count), bits.Unsigned(count);
    }
    if (bits.Unsigned(1)) {
      const int count = bits.Unsigned(5);
      bits.Unsigned(count), bits.Unsigned(count);
    }
    const int translate = bits.Unsigned(5);
    instance.x = bits.Signed(translate);
    instance.y = bits.Signed(translate);
    size_t cursor = bits.Byte();
    if (flags & 8) {
      BitReader color{data_, cursor * 8};
      const uint32_t add = color.Unsigned(1), multiply = color.Unsigned(1);
      const int count = color.Unsigned(4);
      for (uint32_t i = 0; i < (add + multiply) * 4; ++i) color.Unsigned(count);
      cursor = color.Byte();
    }
    if (flags & 0x10) cursor += 2;
    if (flags & 0x20) instances_[reinterpret_cast<const char*>(data_ + cursor)] = instance;
  }

  // Follows the code a player can reach, so bytes an edit jumped over are not read as instructions.
  void WalkActions(size_t start, size_t end) {
    std::vector<size_t> pending{start};
    while (!pending.empty()) {
      size_t at = pending.back();
      pending.pop_back();
      while (at < end && !actions_.count(at)) {
        const uint8_t op = data_[at];
        if (op == 0) break;
        Action action{op, at + 3, 0};
        if (op >= 0x80) action.length = Read<uint16_t>(data_, at + 1);
        const size_t next = op >= 0x80 ? at + 3 + action.length : at + 1;
        if (next > end) {
          valid_ = false;
          return;
        }
        actions_[at] = action;
        if (op == kConstantPool) ReadPool(action);
        if (op == kIf || op == kJump) {
          const size_t target = next + Read<int16_t>(data_, action.body);
          if (target > end) {
            valid_ = false;
            return;
          }
          pending.push_back(target);
          if (op == kJump) break;
        }
        if (op == kDefineFunction || op == kDefineFunction2) {
          const size_t code = FunctionCodeSize(action);
          pending.push_back(next + code);
          WalkActions(next, next + code);
          break;
        }
        at = next;
      }
    }
  }

  size_t FunctionCodeSize(const Action& action) const { return Read<uint16_t>(data_, action.body + action.length - 2); }

  void ReadPool(const Action& action) {
    pool_.clear();
    const uint16_t count = Read<uint16_t>(data_, action.body);
    const char* text = reinterpret_cast<const char*>(data_ + action.body + 2);
    for (int i = 0; i < count; ++i) {
      pool_.emplace_back(text);
      text += pool_.back().size() + 1;
    }
  }

  std::vector<Value> Values(const Action& action) const {
    std::vector<Value> values;
    if (action.op != kPush) return values;
    size_t at = action.body;
    const size_t end = at + action.length;
    while (at < end) {
      const uint8_t type = data_[at++];
      Value value;
      switch (type) {
        case 0: {
          value.kind = Value::kString;
          value.text = reinterpret_cast<const char*>(data_ + at);
          at += value.text.size() + 1;
          break;
        }
        case 1:
          at += 4;
          break;
        case 4:
          at += 1;
          break;
        case 5:
          value.kind = Value::kBool;
          value.number = data_[at++];
          break;
        case 6: {
          uint8_t raw[8];
          std::memcpy(raw, data_ + at + 4, 4);
          std::memcpy(raw + 4, data_ + at, 4);
          value.kind = Value::kNumber;
          value.number = Read<double>(raw, 0);
          at += 8;
          break;
        }
        case 7:
          value.kind = Value::kNumber;
          value.number = Read<int32_t>(data_, at);
          at += 4;
          break;
        case 8:
        case 9: {
          const size_t index = type == 8 ? data_[at] : Read<uint16_t>(data_, at);
          at += type == 8 ? 1 : 2;
          value.kind = Value::kString;
          if (index < pool_.size()) value.text = pool_[index];
          break;
        }
        default:
          break;
      }
      values.push_back(std::move(value));
    }
    return values;
  }

  const uint8_t* data_;
  bool valid_ = true;
  std::map<std::string, Instance> instances_;
  std::map<size_t, Action> actions_;
  std::vector<std::string> pool_;
};

std::string Label(int tab) { return "label0" + std::to_string(tab); }

// The rows a player sees, top to bottom, in the slots the unedited movie authored.
Rows VisibleRows(const Movie& movie, const Rows& titles, const std::vector<int>& unlocked, int last_tab) {
  std::vector<std::pair<int, std::string>> rows;
  for (int tab = 1; tab <= last_tab; ++tab) {
    if (!unlocked.empty() && !unlocked[tab - 1]) continue;
    const auto instance = movie.Instances().find(Label(tab));
    if (instance == movie.Instances().end() || movie.Hides(Label(tab))) continue;
    rows.push_back({instance->second.y, titles[tab - 1]});
  }
  std::sort(rows.begin(), rows.end());
  Rows visible;
  for (const auto& row : rows) visible.push_back(row.second);
  return visible;
}

// handleUp and handleDown: the step wraps between FIRST_TAB and LAST_TAB and passes over locked rows.
int Step(int tab, int direction, int first, int last, const std::vector<int>& unlocked) {
  int at = tab;
  for (int i = 0; i <= last; ++i) {
    at = direction > 0 ? (at == last ? first : at + 1) : (at == 1 ? last : at - 1);
    if (unlocked.empty() || unlocked[at - 1]) return at;
  }
  return tab;
}

Rows Walk(int first, int direction, int last, const Rows& titles, const std::vector<int>& unlocked) {
  Rows visited{titles[first - 1]};
  for (int tab = Step(first, direction, first, last, unlocked); tab != first; tab = Step(tab, direction, first, last, unlocked)) {
    visited.push_back(titles[tab - 1]);
    if (visited.size() > titles.size()) break;
  }
  return visited;
}

const MenuMovie& Find(std::string_view name) {
  return *std::find_if(MenuMovies().begin(), MenuMovies().end(), [&](const MenuMovie& movie) { return movie.name == name; });
}

// Tab dispatch and the footer buttons lie inside these byte ranges of the unedited movie; only the Y branch may change.
bool EditsSpareRange(const MenuMovie& movie, size_t begin, size_t end, size_t allowed) {
  for (const MovieEdit& edit : movie.edits) {
    if (edit.offset == allowed) continue;
    if (edit.offset < end && edit.offset + edit.before.size() / 2 > begin) return false;
  }
  return true;
}

// An edit that replaces code ends with a jump over what is left of it, which must land where a statement began.
bool SkipJumpsLandOnStatements(const MenuMovie& movie, const uint8_t* unedited) {
  constexpr uint8_t kPop = 0x17;
  constexpr size_t kJumpBytes = 5;
  for (const MovieEdit& edit : movie.edits) {
    const size_t bytes = edit.after.size() / 2;
    if (edit.after.size() < 20 || edit.after.substr(edit.after.size() - 10, 6) != "990200") continue;
    const auto after = ParseHex(edit.after);
    const size_t target = edit.offset + bytes + Read<int16_t>(after->data(), bytes - 2);
    const bool after_pop = unedited[target - 1] == kPop;
    const bool after_jump = unedited[target - kJumpBytes] == kJump;
    if (!after_pop && !after_jump) return false;
  }
  return true;
}

struct Bundles {
  std::map<size_t, std::vector<uint8_t>> by_size;
};

bool LoadBundles(Bundles& bundles) {
  std::ifstream in(fs::path(TMNT_ROOT_DIR) / "game/extracted/Data/TMNT.bf", std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> archive((std::istreambuf_iterator<char>(in)), {});
  constexpr size_t kHeader = 5184, kFolder = 12, kRecord = 200, kPayloadHeader = 32, kCompressedHeader = 12;
  const uint32_t count = Read<uint32_t>(archive.data(), kHeader);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* record = archive.data() + kHeader + kFolder + kRecord * i;
    const std::string name(reinterpret_cast<const char*>(record));
    if (name != "f88d0000.bin" && name != "f88d0002.bin") continue;
    const uint32_t offset = Read<uint32_t>(record, 108);
    const uint8_t* block = archive.data() + offset + kPayloadHeader;
    CHECK(std::memcmp(block, "CPBK", 4) == 0);
    const size_t bytes = Read<uint32_t>(block, 8);
    bundles.by_size[bytes] = DecompressLzo1x(block + kCompressedHeader, bytes);
    CHECK(bundles.by_size[bytes].size() == bytes);
  }
  return bundles.by_size.size() == 2;
}

void TestRealMovies() {
  Bundles bundles;
  if (!LoadBundles(bundles)) {
    std::printf("real movies: skipped, game/extracted is missing\n");
    return;
  }
  std::map<size_t, std::vector<uint8_t>> originals = bundles.by_size;

  std::printf("real movies: every listed movie is at its bundle offset and patches once\n");
  for (auto& [size, bundle] : bundles.by_size) {
    CHECK(IsMenuBundle(size));
    const BundlePatchResult first = PatchMenuBundle(bundle.data(), size);
    CHECK(first.failed == 0 && first.patched > 0);
    std::vector<uint8_t> again = bundle;
    const BundlePatchResult second = PatchMenuBundle(again.data(), size);
    CHECK(second.patched == 0 && second.failed == first.patched);
    CHECK(again == bundle);
  }

  std::map<std::string_view, Movie> edited, original;
  for (const MenuMovie& info : MenuMovies()) {
    edited.emplace(info.name, Movie(bundles.by_size[info.bundle_bytes].data() + info.offset, info.bytes));
    original.emplace(info.name, Movie(originals[info.bundle_bytes].data() + info.offset, info.bytes));
    CHECK(edited.at(info.name).Valid());
    CHECK(original.at(info.name).Valid());
    CHECK(SkipJumpsLandOnStatements(info, originals[info.bundle_bytes].data() + info.offset));
  }

  std::printf("main menu: no Leaderboards row, the rows below it close the gap, tabs keep their actions\n");
  const Rows main_titles = {"UNLOCK FULL GAME", "SINGLE PLAYER", "MULTIPLAYER", "HELP & OPTIONS", "LEADERBOARDS", "ACHIEVEMENTS", "EXIT GAME"};
  const Rows main_rows = {"SINGLE PLAYER", "MULTIPLAYER", "HELP & OPTIONS", "ACHIEVEMENTS", "EXIT GAME"};
  const Movie& main = edited.at("menu_main");
  const std::vector<int> main_unlocked = main.UnlockedTable();
  CHECK((main_unlocked == std::vector<int>{0, 1, 1, 1, 0, 1, 1}));
  CHECK(main.LastTabs() == std::set<int>{7});
  CHECK(main.Hides("label05"));
  CHECK(!original.at("menu_main").Hides("label05"));
  CHECK(!main.Reached(0x2982));
  CHECK(VisibleRows(main, main_titles, main_unlocked, 7) == main_rows);
  CHECK(Walk(2, 1, 7, main_titles, main_unlocked) == main_rows);
  CHECK(Step(7, 1, 2, 7, main_unlocked) == 2 && Step(2, -1, 2, 7, main_unlocked) == 7);
  CHECK(Step(4, 1, 2, 7, main_unlocked) == 6 && Step(6, -1, 2, 7, main_unlocked) == 4);
  CHECK(main.Instances().at("label06").y == original.at("menu_main").Instances().at("label05").y);
  CHECK(main.Instances().at("label07").y == original.at("menu_main").Instances().at("label06").y);
  CHECK(main.Instances().at("label06").x == original.at("menu_main").Instances().at("label05").x);
  CHECK(main.Instances().at("label07").x == original.at("menu_main").Instances().at("label06").x);
  CHECK(EditsSpareRange(Find("menu_main"), 0x1DAF, 0x27D6, 0));
  CHECK(EditsSpareRange(Find("menu_main"), 0x29A0, 0x2CF9, 0));

  std::printf("pause menu: no Leaderboards row with and without the sixth row, tabs keep their actions\n");
  const Rows pause_titles = {"RESUME GAME", "HELP & OPTIONS", "LEADERBOARDS", "ACHIEVEMENTS", "EXIT GAME", "STAGE SELECT"};
  const Rows pause_rows = {"RESUME GAME", "HELP & OPTIONS", "ACHIEVEMENTS", "EXIT GAME"};
  const Movie& pause = edited.at("pause");
  const std::vector<int> pause_unlocked = pause.UnlockedTable();
  CHECK((pause_unlocked == std::vector<int>{1, 1, 0, 1, 1, 1}));
  CHECK(pause.LastTabs() == std::set<int>({5, 6}));
  CHECK(pause.Hides("label03"));
  CHECK(VisibleRows(pause, pause_titles, pause_unlocked, 5) == pause_rows);
  CHECK(Walk(1, 1, 5, pause_titles, pause_unlocked) == pause_rows);
  Rows with_stage = pause_rows;
  with_stage.push_back("STAGE SELECT");
  CHECK(Walk(1, 1, 6, pause_titles, pause_unlocked) == with_stage);
  CHECK(Step(5, 1, 1, 5, pause_unlocked) == 1 && Step(1, -1, 1, 5, pause_unlocked) == 5);
  CHECK(Step(2, 1, 1, 5, pause_unlocked) == 4 && Step(4, -1, 1, 5, pause_unlocked) == 2);
  CHECK(pause.Instances().at("label04").y == original.at("pause").Instances().at("label03").y);
  CHECK(pause.Instances().at("label05").y == original.at("pause").Instances().at("label04").y);
  CHECK(pause.Instances().at("label06").y == original.at("pause").Instances().at("label05").y);
  CHECK(pause.Instances().at("label03").y == original.at("pause").Instances().at("label03").y);
  CHECK(EditsSpareRange(Find("pause"), 0x1CC3, 0x26D8, 0));

  std::printf("online menu: no XBOX LIVE PARTY row, the three match rows keep their tabs\n");
  const Rows online_titles = {"QUICK MATCH", "CREATE MATCH", "CUSTOM MATCH", "XBOX LIVE PARTY"};
  const Rows online_rows = {"QUICK MATCH", "CREATE MATCH", "CUSTOM MATCH"};
  const Movie& online = edited.at("online");
  CHECK(online.LastTabs() == std::set<int>{3});
  CHECK(online.Hides("label04"));
  CHECK(VisibleRows(online, online_titles, {}, 3) == online_rows);
  CHECK(Walk(1, 1, 3, online_titles, {}) == online_rows);
  CHECK(Step(3, 1, 1, 3, {}) == 1 && Step(1, -1, 1, 3, {}) == 3);
  CHECK(EditsSpareRange(Find("online"), 0xD5E, 0x1615, 0));

  std::printf("footers: Gamer Card hidden on every screen that reaches it, Y does nothing, B and X stay\n");
  for (std::string_view name : {"lobby", "matchmaking", "summary", "game_over"}) {
    const Movie& movie = edited.at(name);
    CHECK(movie.Hides("gamercard"));
    CHECK(!original.at(name).Hides("gamercard"));
  }
  CHECK(edited.at("lobby").Instances().at("back").x == 15178 - 6657);
  CHECK(edited.at("lobby").Instances().at("invite").x == original.at("lobby").Instances().at("invite").x);
  CHECK(edited.at("lobby").Instances().at("ready").x == original.at("lobby").Instances().at("ready").x);
  CHECK(edited.at("game_over").Instances().at("retry").x == 19007 - 7244);
  CHECK(edited.at("summary").Instances().at("continue_game").x == original.at("summary").Instances().at("continue_game").x);
  const size_t lobby_y_branch = 7865, match_y_branch = 4389, summary_y_branch = 4639, over_y_branch = 4077;
  CHECK(EditsSpareRange(Find("lobby"), 0x1D3B, 0x25CB, lobby_y_branch));
  CHECK(EditsSpareRange(Find("matchmaking"), 0xF56, 0x1621, match_y_branch));
  CHECK(EditsSpareRange(Find("summary"), 0x10C7, 0x1739, summary_y_branch));
  CHECK(EditsSpareRange(Find("game_over"), 0xE95, 0x1443, over_y_branch));
  const uint8_t* lobby = bundles.by_size[Find("lobby").bundle_bytes].data() + Find("lobby").offset;
  CHECK(Read<uint8_t>(lobby, lobby_y_branch - 3) == kIf && Read<int16_t>(lobby, lobby_y_branch) == 0);
}

}  // namespace

int main() {
  TestEditRules();
  TestTable();
  TestGamerCardIsGone();
  TestRealMovies();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
