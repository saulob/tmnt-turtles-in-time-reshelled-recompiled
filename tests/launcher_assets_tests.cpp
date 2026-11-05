// Launcher header art: found next to the executable whatever the working directory, shipped by the build and the
// release package, and loaded without the game's runtime.
// Run: tmnt_launcher_assets_tests

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "launcher_assets.h"

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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

void TestPathResolution() {
  std::printf("header art path is relative to the executable folder\n");
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path root = std::filesystem::temp_directory_path() / ("tmnt-assets-tests-" + std::to_string(now));
  const std::filesystem::path executable_folder = root / "install with space";
  const std::filesystem::path elsewhere = root / "elsewhere";
  std::error_code ec;
  std::filesystem::create_directories(executable_folder / "assets" / "launcher", ec);
  std::filesystem::create_directories(elsewhere, ec);
  { std::ofstream(executable_folder / "assets" / "launcher" / "header-art.png", std::ios::binary) << "art"; }

  const std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::current_path(elsewhere, ec);
  CHECK(!ec);
  const std::filesystem::path art = tmnt::HeaderArtPath(executable_folder);
  CHECK(art.is_absolute());
  CHECK(std::filesystem::is_regular_file(art));
  CHECK(!std::filesystem::exists(std::filesystem::path("assets") / "launcher" / "header-art.png"));
  std::filesystem::current_path(previous, ec);
  std::filesystem::remove_all(root, ec);
}

void TestWiring() {
  std::printf("the launcher, the build and the release package carry the header art\n");
  const std::string source_dir = TMNT_SRC_DIR;
  const std::string launcher = ReadFile(source_dir + "/launcher.cpp");
  CHECK(Has(launcher, "HeaderArtPath(rex::filesystem::GetExecutableFolder())"));
  CHECK(Has(launcher, "header_art_ = LoadHeaderArt(paths_.immediate_drawer);"));
  CHECK(!Has(launcher, "\"assets/launcher/header-art.png\""));

  const std::string cmake = ReadFile(source_dir + "/../CMakeLists.txt");
  CHECK(Has(cmake, "TMNT_STAGED_HEADER_ART"));
  const std::string package = ReadFile(source_dir + "/../tools/package-release.ps1");
  CHECK(Has(package, "$AssetFiles = @('assets\\launcher\\header-art.png')"));
  const std::string linux_package = ReadFile(source_dir + "/../tools/package-linux.sh");
  CHECK(Has(linux_package, "usr/bin/assets/launcher/header-art.png"));
  CHECK(std::filesystem::is_regular_file(source_dir + "/../assets/launcher/header-art.png"));
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestPathResolution();
  TestWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
