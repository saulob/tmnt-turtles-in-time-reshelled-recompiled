// Launcher images (header art, sponsor heart): found next to the executable whatever the working directory, shipped by
// the build and the release package, and loaded without the game's runtime.
// Run: tmnt_launcher_assets_tests

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "embedded_file.h"
#include "launcher_assets.h"
#include "page_icons.h"

namespace tmnt {
extern const EmbeddedFile kPageIconFiles[];
}

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

struct IconExpectation {
  tmnt::LauncherPage page;
  const char* file;
  uint64_t signature;
  float scale;
};

uint64_t Signature(const std::string& bytes) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char c : bytes) {
    if (c == '\r') continue;
    hash ^= uint8_t(c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

void TestSidebarIcons() {
  std::printf("every page has the expected embedded sidebar icon\n");
  const std::string source_dir = TMNT_SRC_DIR;
  const std::string root = source_dir + "/..";
  const std::string launcher = ReadFile(source_dir + "/launcher.cpp");
  CHECK(Has(launcher, "#include \"page_icons.h\""));
  CHECK(Has(launcher, "result.icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };"));

  // FNV-1a of the file with CR removed. The shared values match Dragon's launcher icon set.
  const IconExpectation expected[] = {
      {tmnt::LauncherPage::kPlay, "play.svg", 0x4d0af5ebd60af49bull, 0.84f},
      {tmnt::LauncherPage::kDisplay, "display.svg", 0x28b392b11cfe8c30ull, 0.90f},
      {tmnt::LauncherPage::kGraphics, "graphics.svg", 0x0e5b5a9f96fc33e8ull, 0.84f},
      {tmnt::LauncherPage::kGameplay, "gameplay.svg", 0xbb3ef579d120ec64ull, 0.92f},
      {tmnt::LauncherPage::kSnes, "snes.svg", 0xa81819cce4b72030ull, 0.92f},
      {tmnt::LauncherPage::kCheats, "cheats.svg", 0xf82376053693ffaaull, 0.92f},
      {tmnt::LauncherPage::kOnline, "online.svg", 0xb94beeac22b2c389ull, 0.90f},
      {tmnt::LauncherPage::kAchievements, "achievements.svg", 0x51faa0a39512120full, 0.86f},
      {tmnt::LauncherPage::kControls, "controls.svg", 0x43cca1bdf709820cull, 0.94f},
      {tmnt::LauncherPage::kBugReport, "bug-report.svg", 0xa03ce8a040a832d0ull, 0.90f},
      {tmnt::LauncherPage::kAbout, "about.svg", 0x6214cc4745067de1ull, 0.88f},
      {tmnt::LauncherPage::kDlc, "dlc.svg", 0x55c36914987bf375ull, 0.88f},
  };
  for (const IconExpectation& icon : expected) {
    const tmnt::PageIconAsset* asset = tmnt::FindPageIcon(icon.page);
    CHECK(asset && std::string(asset->file) == icon.file);
    CHECK(asset && std::abs(asset->optical_scale - icon.scale) < 0.0001f);
    const std::string path = root + "/assets/launcher/icons/" + icon.file;
    const std::string bytes = ReadFile(path);
    CHECK(!bytes.empty() && Has(bytes, "<svg"));
    CHECK(Signature(bytes) == icon.signature);
    const std::vector<uint8_t> rgba = tmnt::RenderPageIcon(icon.page, 32);
    CHECK(rgba.size() == 32u * 32u * 4u);
    bool opaque = false;
    for (size_t i = 3; i < rgba.size(); i += 4) opaque = opaque || rgba[i] != 0;
    CHECK(opaque);
  }

  size_t last = 0;
  for (const char* page : {"kPlay", "kDisplay", "kGraphics", "kGameplay", "kSnes", "kCheats", "kOnline",
                           "kAchievements", "kControls", "kBugReport", "kAbout"}) {
    const size_t at = launcher.find(std::string("MakePage(LauncherPage::") + page);
    CHECK(at != std::string::npos && at >= last);
    last = at;
  }
  CHECK(Has(launcher, "MakePage(LauncherPage::kDlc"));

  CHECK(!std::filesystem::exists(std::filesystem::path(root) / "super-nintendo-controller-svgrepo-com.svg"));
  CHECK(std::filesystem::is_regular_file(
      std::filesystem::path(root) / "assets/launcher/icons/source/super-nintendo-controller-svgrepo-com.svg"));
  CHECK(!std::filesystem::exists(std::filesystem::path(root) / "assets/launcher/icons/trophies.svg"));

  for (const IconExpectation& icon : expected) {
    bool embedded = false;
    for (const tmnt::EmbeddedFile* file = tmnt::kPageIconFiles; file->name; ++file) {
      if (std::string(file->name) == icon.file && file->data && file->size) embedded = true;
    }
    CHECK(embedded);
  }
  bool stale_achievement_icon = false;
  bool embedded_source_file = false;
  for (const tmnt::EmbeddedFile* file = tmnt::kPageIconFiles; file->name; ++file) {
    stale_achievement_icon = stale_achievement_icon || std::string(file->name) == "trophies.svg";
    embedded_source_file = embedded_source_file || std::string(file->name) == "super-nintendo-controller-svgrepo-com.svg";
  }
  CHECK(!stale_achievement_icon);
  CHECK(!embedded_source_file);

  const std::string sources = ReadFile(root + "/assets/launcher/icons/SOURCES.md");
  CHECK(Has(sources, "dragons-crown-pro-pc/assets/launcher/icons"));
  CHECK(Has(sources, "`achievements.svg` | Achievements"));
  CHECK(Has(sources, "`cheats.svg` | Star Alt 1"));
  CHECK(Has(sources, "`snes.svg` | Super Nintendo Controller"));
  CHECK(Has(sources, "assets/launcher/icons/source/super-nintendo-controller-svgrepo-com.svg"));
  CHECK(Has(sources, "https://www.svgrepo.com/svg/82749/super-nintendo-controller"));
  CHECK(Has(sources, "| CC0 |"));
  CHECK(!Has(sources, "`trophies.svg`"));
  CHECK(!Has(sources, "Sparkling star"));
  CHECK(!Has(sources, "SNES cartridge"));

  const std::string licenses = ReadFile(root + "/packaging/licenses/THIRD-PARTY-LICENSES.txt");
  CHECK(Has(licenses, "| achievements.svg | Achievements |"));
  CHECK(Has(licenses, "| cheats.svg | Star Alt 1 |"));
  CHECK(Has(licenses, "| snes.svg | Super Nintendo Controller |"));
  CHECK(Has(licenses, "assets/launcher/icons/source/super-nintendo-controller-svgrepo-com.svg"));
  CHECK(Has(licenses, "https://www.svgrepo.com/svg/82749/super-nintendo-controller"));
  CHECK(Has(licenses, "| CC0 |"));
  CHECK(!Has(licenses, "| trophies.svg |"));

  const std::string cmake = ReadFile(root + "/CMakeLists.txt");
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
