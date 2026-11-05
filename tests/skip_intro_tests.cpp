// Skip intro: default, persistence, Gameplay row, diagnostics, and the real boot path. The world links below are the ones
// the game's data authors; walking them proves the movies a boot enters with the setting Off and On. The hook's guest
// assumptions, including that Universe_Init is the only code naming the Logo world, are checked against the game image.
// Run: tmnt_skip_intro_tests

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "skip_intro.h"

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

constexpr uint32_t kIntroWorld = 0x37400C4A;
constexpr uint32_t kOutroWorld = 0x374034EE;
constexpr uint32_t kAnyOtherWorld = 0x37400001;

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(TMNT_ROOT_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The next world of a movie world, or 0 for any other world.
uint32_t MovieWorldNext(uint32_t world) {
  switch (world) {
    case tmnt::kLogoWorld:
      return kIntroWorld;
    case kIntroWorld:
      return tmnt::kStartMenuWorld;
    case kOutroWorld:
      return tmnt::kStartMenuWorld;
    default:
      return 0;
  }
}

std::vector<uint32_t> MovieWorldsBeforeTitle(bool skip_intro) {
  std::vector<uint32_t> played;
  uint32_t world = tmnt::StartupWorldTarget(tmnt::kLogoWorld, skip_intro);  // Universe_Init's transition
  while (uint32_t next = MovieWorldNext(world)) {
    played.push_back(world);
    world = tmnt::StartupWorldTarget(next, skip_intro);
  }
  CHECK(world == tmnt::kStartMenuWorld);
  return played;
}

void TestSetting() {
  std::printf("setting: Off by default, saved with the player settings, shown in diagnostics\n");
  const std::string settings = ReadFile("src/settings.cpp");
  CHECK(Has(ReadFile("src/settings.h"), "REXCVAR_DECLARE(bool, tmnt_skip_intro);"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_skip_intro, false,"));
  const size_t keys = settings.find("std::vector<std::string> GameSettingKeys() {");
  CHECK(keys != std::string::npos && settings.find("\"tmnt_skip_intro\",", keys) != std::string::npos);
  CHECK(Has(settings, "diagnostics.launcher_gameplay.skip_intro = OnOff(settings.GetBool(\"tmnt_skip_intro\", false));"));
  CHECK(Has(ReadFile("src/diagnostics.h"), "std::string skip_intro;"));
  CHECK(Has(ReadFile("src/diagnostics.cpp"), "\" | Skip intro=\" + diagnostics.launcher_gameplay.skip_intro"));
}

void TestGameplayRow() {
  std::printf("launcher: Gameplay row, ahead of the frame rate rows\n");
  const std::string launcher = ReadFile("src/launcher.cpp");
  const size_t page = launcher.find("static void PageGameplay(trg::Ui& ui) {");
  const size_t skip = launcher.find(
      "ui.Toggle(\"Skip intro\", \"Start directly at the title screen\", \"tmnt_skip_intro\", false, \"Off\", \"On\");");
  CHECK(page != std::string::npos && skip != std::string::npos);
  CHECK(page < skip && skip < launcher.find("\"tmnt_show_fps\", true", page));
}

void TestGate() {
  std::printf("gate: only the setting's own true value\n");
  CHECK(tmnt::SkipIntroEnabled("true"));
  CHECK(!tmnt::SkipIntroEnabled("false"));
  CHECK(!tmnt::SkipIntroEnabled(""));
}

void TestStartupTarget() {
  std::printf("target: Off keeps the Logo world, On goes to the title\n");
  CHECK(tmnt::StartupWorldTarget(tmnt::kLogoWorld, false) == tmnt::kLogoWorld);
  CHECK(tmnt::StartupWorldTarget(tmnt::kLogoWorld, true) == tmnt::kStartMenuWorld);
}

void TestBootPath() {
  std::printf("boot path: every startup movie world with the setting Off, none with it On\n");
  const std::vector<uint32_t> off = MovieWorldsBeforeTitle(false);
  CHECK(off == std::vector<uint32_t>({tmnt::kLogoWorld, kIntroWorld}));
  CHECK(MovieWorldsBeforeTitle(true).empty());
}

void TestLaterWorldsUnaffected() {
  std::printf("later worlds: every other target passes through, setting On or Off\n");
  for (uint32_t world : {kIntroWorld, kOutroWorld, kAnyOtherWorld, tmnt::kStartMenuWorld, 0u, 0xFFFFFFFFu}) {
    CHECK(tmnt::StartupWorldTarget(world, true) == world);
    CHECK(tmnt::StartupWorldTarget(world, false) == world);
  }
}

void TestHook() {
  std::printf("hook: rewrites the target before the transition, nothing else\n");
  const std::string hook = ReadFile("src/skip_intro_tmnt_r.cpp");
  CHECK(Has(hook, "REX_HOOK_RAW(sub_87602DA8) {"));
  CHECK(Has(ReadFile("CMakeLists.txt"), "src/skip_intro_tmnt_r.cpp"));
  const size_t gate = hook.find("StartupWorldTarget(next, SkipIntroEnabled(rex::cvar::GetFlagByName(\"tmnt_skip_intro\")))");
  const size_t store = hook.find("StoreBE32(base, universe + kNextWorldToLoad, target);");
  const size_t original = hook.find("__imp__sub_87602DA8(ctx, base);");
  CHECK(gate != std::string::npos && store != std::string::npos && original != std::string::npos);
  CHECK(gate < store && store < original);
  CHECK(Has(hook, "if (target != next) {"));
  for (const char* banned : {"sub_876B1B00", "kCurrentMovie", "kNextLoadingScreen, ", "Button", "Sleep", "Time", "Skippable"}) {
    CHECK(!Has(hook, banned));  // no movie world state, input or clock trick; the loading world stays
  }
}

class GameImage {
 public:
  explicit GameImage(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  bool loaded() const { return !bytes_.empty(); }
  size_t words() const { return bytes_.size() / 4; }
  uint32_t at_index(size_t index) const {
    uint32_t value;
    std::memcpy(&value, bytes_.data() + index * 4, 4);
    return std::byteswap(value);
  }
  uint32_t word(uint32_t address) const {
    const size_t index = (address - kBase) / 4;
    return index < words() ? at_index(index) : 0;
  }
  static constexpr uint32_t kBase = 0x87500000;

 private:
  std::vector<char> bytes_;
};

// Addresses that assemble `key` with lis rS,high and a following ori rA,rS,low.
std::vector<uint32_t> KeyBuilders(const GameImage& image, uint32_t key) {
  std::vector<uint32_t> found;
  constexpr uint32_t kOri = 0x60000000, kImmediateAndOpcode = 0xFC00FFFF;
  constexpr size_t kReach = 48;
  for (size_t i = 0; i < image.words(); ++i) {
    const uint32_t word = image.at_index(i);
    if ((word & kImmediateAndOpcode) != (kOri | (key & 0xFFFF))) continue;
    const uint32_t lis = 0x3C000000u | ((word >> 21) & 31) << 21 | key >> 16;
    for (size_t j = i > kReach ? i - kReach : 0; j < i; ++j) {
      if (image.at_index(j) == lis) {
        found.push_back(GameImage::kBase + uint32_t(i * 4));
        break;
      }
    }
  }
  return found;
}

void TestGameImage() {
  std::printf("game image: Universe_Init, Universe_NextWorldNow and the only code naming the Logo world\n");
  const GameImage image(std::filesystem::path(TMNT_ROOT_DIR) / "game/analysis/tmnt_r.image.bin");
  if (!image.loaded()) {
    std::printf("  skipped: game/analysis/tmnt_r.image.bin is missing\n");
    return;
  }
  constexpr uint32_t kMflrR12 = 0x7D8802A6;
  CHECK(image.word(0x876036E0) == kMflrR12);  // Universe_Init
  CHECK(image.word(0x87603AE8) == 0x3D203740);  // lis r9,0x3740
  CHECK(image.word(0x87603AF0) == 0x612734E5);  // ori r7,r9,0x34E5: LOGO_WORLD
  CHECK(image.word(0x87603B18) == 0x90EB1C2C);  // stw r7,7212(r11): m_NextWorldToLoad
  CHECK(image.word(0x87603B2C) == 0x4BFFF27D);  // bl Universe_NextWorldNow

  CHECK(image.word(0x87602DA8) == kMflrR12);  // Universe_NextWorldNow
  CHECK(image.word(0x87602DEC) == 0x817F0008);  // lwz r11,8(r31): the universe vars
  CHECK(image.word(0x87602DF0) == 0x810B1C28);  // lwz r8,7208(r11): m_NextLoadingScreen
  CHECK(image.word(0x87602E30) == 0x83CB1C2C);  // lwz r30,7212(r11): m_NextWorldToLoad

  const std::vector<uint32_t> logo = KeyBuilders(image, tmnt::kLogoWorld);
  CHECK(logo == std::vector<uint32_t>({0x87603AF0}));  // the skip cannot fire on any transition after boot
  CHECK(!KeyBuilders(image, tmnt::kStartMenuWorld).empty());
}

}  // namespace

int main() {
  TestSetting();
  TestGameplayRow();
  TestGate();
  TestStartupTarget();
  TestBootPath();
  TestLaterWorldsUnaffected();
  TestHook();
  TestGameImage();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
