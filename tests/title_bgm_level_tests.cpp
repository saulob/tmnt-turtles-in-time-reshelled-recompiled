// Title music level: the level heard before the profile loads equals the level the profile brings at Press START, for
// every saved state. The model below follows the game's SoundBGM and BasicWorld_LoadSettings scripts; the guest words
// the hook depends on are checked against the game image when it is present.
// Run: tmnt_title_bgm_level_tests

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "title_bgm_level.h"

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

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(TMNT_ROOT_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

std::vector<uint8_t> Blob(int32_t level, size_t size = 92) {
  std::vector<uint8_t> blob(size, 0);
  if (size >= tmnt::kSavedMusicLevelOffset + 4) {
    const uint32_t value = std::byteswap(uint32_t(level));
    std::memcpy(blob.data() + tmnt::kSavedMusicLevelOffset, &value, 4);
  }
  return blob;
}

// SoundBGM_ChangeVolumeWithoutSaving at the authored 0 dB: -20 - unit * level, unit = (-20 - 0) / 10.
double LevelDb(int32_t level) { return level > 0 ? -20.0 + 2.0 * level : -80.0; }

// One boot: the universe default, the first SoundBGM_Init (with the hook's store), then BasicWorld_LoadSettings at Start.
struct BootVolumes {
  double title_db;
  double after_start_db;
};

BootVolumes Boot(const std::optional<int32_t>& seed, int32_t saved_level) {
  int32_t m_bgm_volume = tmnt::kDefaultMusicLevel;
  if (seed) m_bgm_volume = *seed;
  const double title = LevelDb(m_bgm_volume);
  m_bgm_volume = saved_level;
  return {title, LevelDb(m_bgm_volume)};
}

// The game's reading of the blob: a stored setting holds the level, an unset one gives the default.
int32_t GameLevel(const std::vector<uint8_t>& blob) {
  if (blob.size() < tmnt::kSavedMusicLevelOffset + 4) return tmnt::kDefaultMusicLevel;
  uint32_t value;
  std::memcpy(&value, blob.data() + tmnt::kSavedMusicLevelOffset, 4);
  return int32_t(std::byteswap(value));
}

void TestSavedLevel() {
  std::printf("saved level: offset 76, big-endian, 0-10, anything else is the default\n");
  for (int32_t level = 0; level <= 10; ++level) CHECK(tmnt::SavedMusicLevel(Blob(level)) == level);
  CHECK(tmnt::SavedMusicLevel(Blob(11)) == 10);
  CHECK(tmnt::SavedMusicLevel(Blob(-1)) == 10);
  CHECK(tmnt::SavedMusicLevel(Blob(0x0A000000)) == 10);
  CHECK(tmnt::SavedMusicLevel(Blob(6, 79)) == 10);
  CHECK(tmnt::SavedMusicLevel(Blob(6, 80)) == 6);
  CHECK(tmnt::SavedMusicLevel({}) == 10);
}

void TestNoJumpAtPressStart() {
  std::printf("invariant: no volume change at Press START, for every saved state\n");
  std::vector<std::vector<uint8_t>> states{{}, Blob(0, 40)};  // never saved, unset, then every saved level
  for (int32_t level = 0; level <= 10; ++level) states.push_back(Blob(level));
  for (const std::vector<uint8_t>& blob : states) {
    const std::string published = std::to_string(tmnt::SavedMusicLevel(blob));
    const BootVolumes volumes = Boot(tmnt::TitleMusicSeed(published), GameLevel(blob));
    CHECK(volumes.title_db == volumes.after_start_db);
  }
}

void TestRegressionShape() {
  std::printf("regression: a fixed seed only matches the one profile it was measured on\n");
  const BootVolumes saved_10 = Boot(6, 10);  // the online instances' profile, and every fresh install
  CHECK(saved_10.title_db == -8.0 && saved_10.after_start_db == 0.0);
  CHECK(std::abs(std::pow(10.0, (saved_10.after_start_db - saved_10.title_db) / 20.0) - 2.51) < 0.01);  // heard as 2x
  const BootVolumes saved_4 = Boot(6, 4);
  CHECK(saved_4.title_db != saved_4.after_start_db);
  const BootVolumes saved_6 = Boot(6, 6);
  CHECK(saved_6.title_db == saved_6.after_start_db);  // why it looked fixed
}

void TestSeed() {
  std::printf("seed: only a clean published level is stored\n");
  CHECK(tmnt::TitleMusicSeed("0") == 0);
  CHECK(tmnt::TitleMusicSeed("6") == 6);
  CHECK(tmnt::TitleMusicSeed("10") == 10);
  CHECK(!tmnt::TitleMusicSeed(""));
  CHECK(!tmnt::TitleMusicSeed("11"));
  CHECK(!tmnt::TitleMusicSeed("-1"));
  CHECK(!tmnt::TitleMusicSeed("6x"));
  CHECK(!tmnt::TitleMusicSeed("music"));
  const BootVolumes unset = Boot(tmnt::TitleMusicSeed(""), 10);
  CHECK(unset.title_db == 0.0);  // nothing published: the game's own default
}

void TestFile() {
  std::printf("file: read from the real path, a missing file is the default\n");
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "tmnt_title_bgm_level_test";
  fs::remove_all(root);
  const fs::path file = tmnt::SavedSettingsPath(root, 0x584109D2);
  CHECK(file == root / "584109D2" / "profile" / "User" / "63E83FFF");
  CHECK(tmnt::ReadSavedMusicLevel(file) == 10);
  fs::create_directories(file.parent_path());
  const std::vector<uint8_t> blob = Blob(4);
  {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
  }
  CHECK(tmnt::ReadSavedMusicLevel(file) == 4);
  fs::remove_all(root);
}

void TestHookShape() {
  std::printf("hook: seeds once, before the original, from the published level, with no early profile access\n");
  const std::string hook = ReadFile("src/title_bgm_level_tmnt_r.cpp");
  const size_t seed = hook.find("TitleMusicSeed(rex::cvar::GetFlagByName(\"tmnt_title_music_level\"))");
  const size_t store = hook.find("StoreBE32(base, ctx.r3.u32 + kUniverseBgmVolumeOffset, uint32_t(*level));");
  const size_t original = hook.find("__imp__sub_87608560(ctx, base);");
  CHECK(Contains(hook, "REX_HOOK_RAW(sub_87608560)"));
  CHECK(Contains(hook, "seeded.exchange(true)"));
  CHECK(seed != std::string::npos && store != std::string::npos && original != std::string::npos);
  CHECK(seed < store && store < original);
  CHECK(!Contains(hook, "kTitleMusicLevel"));
  const std::string publisher = ReadFile("src/title_bgm_level.cpp");
  for (const std::string* text : {&hook, &publisher}) {
    for (const char* banned : {"kernel_state", "GetSetting", "Serialize", "UserProfile", "GetProcAddress", "GetModuleHandle"}) {
      CHECK(!Contains(*text, banned));
    }
  }
  const std::string app = ReadFile("src/tmnt_turtles_in_time_reshelled_app.h");
  const size_t publish = app.find("tmnt::PublishTitleMusicLevel(user_data_root());");
  CHECK(publish != std::string::npos && publish < app.find("ReXApp::LaunchModule();"));
  CHECK(Contains(ReadFile("CMakeLists.txt"), "src/title_bgm_level.cpp"));
}

void TestGameWords() {
  std::printf("game image: the fields the hook relies on\n");
  std::ifstream in(std::filesystem::path(TMNT_ROOT_DIR) / "game/analysis/tmnt_r.image.bin", std::ios::binary);
  if (!in) {
    std::printf("  skipped: game/analysis/tmnt_r.image.bin is missing\n");
    return;
  }
  const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto word = [&](uint32_t at) {
    const size_t offset = at - 0x87500000u;
    if (offset + 4 > bytes.size()) return 0u;
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return std::byteswap(value);
  };
  constexpr uint32_t kMflrR12 = 0x7D8802A6;
  for (uint32_t entry : {0x87608560u, 0x87608280u, 0x87607E18u, 0x87607F18u}) CHECK(word(entry) == kMflrR12);
  CHECK(word(0x87608318) == 0x8083001C);  // UI_InitVolume: lwz r4,28(r3), m_BGMVolume
  CHECK(word(0x87607FB4) == 0x93C3001C);  // SoundBGM_changeVolume: stw r30,28(r3)
}

}  // namespace

int main() {
  TestSavedLevel();
  TestNoJumpAtPressStart();
  TestRegressionShape();
  TestSeed();
  TestFile();
  TestHookShape();
  TestGameWords();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
