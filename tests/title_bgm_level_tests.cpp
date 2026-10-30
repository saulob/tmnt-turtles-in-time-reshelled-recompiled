// Title BGM workaround source checks. Run: tmnt_title_bgm_level_tests

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

void TestFirstInitSeedsTheUniverseOnce() {
  std::printf("hook: first SoundBGM_Init seeds universe +28 with 6, once, before the original\n");
  const std::string hook = ReadFile("src/title_bgm_level_tmnt_r.cpp");
  const size_t store = hook.find("StoreBE32(base, ctx.r3.u32 + kUniverseBgmVolumeOffset, kTitleMusicLevel);");
  const size_t original = hook.find("__imp__sub_87608560(ctx, base);");
  CHECK(Contains(hook, "REX_HOOK_RAW(sub_87608560)"));
  CHECK(Contains(hook, "kUniverseImport = 5;"));
  CHECK(Contains(hook, "kUniverseBgmVolumeOffset = 28;"));
  CHECK(Contains(hook, "kTitleMusicLevel = 6;"));
  CHECK(Contains(hook, "seeded.exchange(true)"));
  CHECK(store != std::string::npos && original != std::string::npos && store < original);
}

void TestHookHasNoOtherDependencies() {
  std::printf("hook: no profile, bridge, STL or audio calls\n");
  const std::string hook = ReadFile("src/title_bgm_level_tmnt_r.cpp");
  for (const char* banned : {"kernel_state", "GetSetting", "Serialize", "UserProfile", "GetProcAddress",
                             "GetModuleHandle", "TmntInitialMusicLevel", "std::vector", "std::string",
                             "sub_87607E18", "sub_87607F18", "sub_87608280", "SND_Setf", "ChangeVolume"})
    CHECK(!Contains(hook, banned));
}

void TestOtherCodeIsUntouched() {
  std::printf("tree: Main track not hooked, bridge and profile files gone\n");
  const std::string hook = ReadFile("src/title_bgm_level_tmnt_r.cpp");
  const std::string cmake = ReadFile("CMakeLists.txt");
  CHECK(!Contains(hook, "REX_HOOK_RAW(sub_87608620)"));
  CHECK(!std::filesystem::exists(std::filesystem::path(TMNT_ROOT_DIR) / "src/initial_music_level.cpp"));
  CHECK(!std::filesystem::exists(std::filesystem::path(TMNT_ROOT_DIR) / "src/bgm_level.h"));
  CHECK(!std::filesystem::exists(std::filesystem::path(TMNT_ROOT_DIR) / "src/debug_saulo_bgm_tmnt_r.cpp"));
  CHECK(!Contains(cmake, "initial_music_level"));
  CHECK(Contains(cmake, "src/title_bgm_level_tmnt_r.cpp"));
}

}  // namespace

int main() {
  TestFirstInitSeedsTheUniverseOnce();
  TestHookHasNoOtherDependencies();
  TestOtherCodeIsUntouched();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
