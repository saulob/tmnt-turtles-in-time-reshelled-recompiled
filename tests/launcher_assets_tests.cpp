// Launcher images (header art, sponsor heart): found next to the executable whatever the working directory, shipped by
// the build and the release package, and loaded without the game's runtime.
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
  std::printf("launcher image paths are relative to the executable folder\n");
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
  const std::filesystem::path art = tmnt::LauncherImagePath(executable_folder, "header-art.png");
  CHECK(art.is_absolute());
  CHECK(std::filesystem::is_regular_file(art));
  CHECK(!std::filesystem::exists(std::filesystem::path("assets") / "launcher" / "header-art.png"));
  CHECK(tmnt::LauncherImagePath(executable_folder, "sponsor-heart.png") ==
        executable_folder / "assets" / "launcher" / "sponsor-heart.png");
  std::filesystem::current_path(previous, ec);
  std::filesystem::remove_all(root, ec);
}

void TestWiring() {
  std::printf("the launcher, the build and the release package carry the header art and the sponsor heart\n");
  const std::string source_dir = TMNT_SRC_DIR;
  const std::string launcher = ReadFile(source_dir + "/launcher.cpp");
  CHECK(Has(launcher, "LauncherImagePath(rex::filesystem::GetExecutableFolder(), file)"));
  CHECK(Has(launcher, "header_art_ = LoadLauncherImage(paths_.immediate_drawer, \"header-art.png\");"));
  CHECK(Has(launcher, "sponsor_icon_ = LoadLauncherImage(paths_.immediate_drawer, \"sponsor-heart.png\", true);"));
  CHECK(!Has(launcher, "\"assets/launcher/header-art.png\""));

  const std::string cmake = ReadFile(source_dir + "/../CMakeLists.txt");
  CHECK(Has(cmake, "TMNT_STAGED_HEADER_ART"));
  CHECK(Has(cmake, "TMNT_STAGED_SPONSOR_HEART"));
  const std::string package = ReadFile(source_dir + "/../tools/package-release.ps1");
  CHECK(Has(package, "$AssetFiles = @('assets\\launcher\\header-art.png', 'assets\\launcher\\sponsor-heart.png')"));
  const std::string linux_package = ReadFile(source_dir + "/../tools/linux-policy.sh");
  CHECK(Has(linux_package, "usr/bin/assets/launcher/header-art.png"));
  CHECK(Has(linux_package, "usr/bin/assets/launcher/sponsor-heart.png"));
  CHECK(std::filesystem::is_regular_file(source_dir + "/../assets/launcher/header-art.png"));
  CHECK(std::filesystem::is_regular_file(source_dir + "/../assets/launcher/sponsor-heart.png"));
}

void TestSidebarIcons() {
  std::printf("every page has an embedded sidebar icon\n");
  const std::string source_dir = TMNT_SRC_DIR;
  const std::string launcher = ReadFile(source_dir + "/launcher.cpp");
  CHECK(Has(launcher, "#include \"page_icons.h\""));
  CHECK(Has(launcher, "result.icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kPlay"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kDisplay"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kGraphics"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kGameplay"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kOnline"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kAchievements"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kControls"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kBugReport"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kAbout"));
  CHECK(Has(launcher, "MakePage(LauncherPage::kDlc"));

  const std::string icons = ReadFile(source_dir + "/page_icons.cpp");
  CHECK(Has(icons, "{LauncherPage::kPlay, \"play.svg\""));
  CHECK(Has(icons, "{LauncherPage::kAchievements, \"trophies.svg\""));
  CHECK(Has(icons, "{LauncherPage::kDlc, \"dlc.svg\""));

  for (const char* file :
       {"play.svg", "display.svg", "graphics.svg", "gameplay.svg", "online.svg", "controls.svg", "trophies.svg",
        "bug-report.svg", "about.svg", "dlc.svg"}) {
    CHECK(std::filesystem::is_regular_file(source_dir + "/../assets/launcher/icons/" + file));
  }
  const std::string cmake = ReadFile(source_dir + "/../CMakeLists.txt");
  CHECK(Has(cmake, "src/page_icons.cpp"));
  CHECK(Has(cmake, "src/embed_files.cmake"));
  CHECK(Has(cmake, "assets/launcher/icons/*.svg"));
  CHECK(Has(launcher, "c.render_scale = g_font_density;"));
  CHECK(Has(launcher, "launcher_->config().render_scale = g_font_density;"));
  CHECK(Has(launcher, "g_fonts.size = trg::layout::kTextBody;"));
  CHECK(Has(launcher, "page_icons_[std::make_pair(page, physical_pixels)]"));
  CHECK(Has(launcher, "CreateTexture(uint32_t(physical_pixels), uint32_t(physical_pixels)"));
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestPathResolution();
  TestWiring();
  TestSidebarIcons();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
