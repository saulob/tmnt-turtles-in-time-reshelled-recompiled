// Internal render resolution is gone: the launcher row, the setting, the source, the configuration, the docs, the
// translations and patches 0036-0038, with nothing left that needs them. Render resolution 1x-4x stays.
// Run: tmnt_internal_render_removed_tests

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "aspect_ratio.h"

namespace {

namespace fs = std::filesystem;

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

const fs::path kRoot = TMNT_ROOT_DIR;

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

std::vector<fs::path> FilesUnder(const fs::path& folder, std::initializer_list<const char*> extensions) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(kRoot / folder, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file()) {
      continue;
    }
    const std::string extension = it->path().extension().string();
    if (std::any_of(extensions.begin(), extensions.end(), [&](const char* want) { return extension == want; })) {
      files.push_back(it->path());
    }
  }
  return files;
}

std::vector<fs::path> ProjectText() {
  std::vector<fs::path> files = FilesUnder("src", {".cpp", ".h"});
  for (const fs::path& file : FilesUnder("config", {".toml"})) files.push_back(file);
  for (const fs::path& file : FilesUnder("docs", {".md"})) {
    if (file.filename() != "cross-project-parity-audit.md") files.push_back(file);
  }
  for (const char* file : {"CMakeLists.txt", "README.md"}) files.push_back(kRoot / file);
  return files;
}

void TestFilesAndPatchesGone() {
  std::printf("files and patches\n");
  CHECK(!fs::exists(kRoot / "src" / "internal_render_scale.h"));
  CHECK(!fs::exists(kRoot / "tests" / "internal_render_scale_tests.cpp"));

  int removed = 0;
  int kept = 0;
  for (const auto& entry : fs::directory_iterator(kRoot / "patches" / "rexglue-sdk")) {
    const std::string name = entry.path().filename().string();
    removed += name.starts_with("0036-") || name.starts_with("0037-") || name.starts_with("0038-");
    kept += name.ends_with(".patch");
  }
  CHECK(removed == 0);
  CHECK(kept > 60);

  const std::string manifest = ReadFile(kRoot / "patches" / "rexglue-sdk" / "patches.txt");
  CHECK(!Has(manifest, "internal-render-scale"));
  CHECK(!std::regex_search(manifest, std::regex("(^|\\n)003[678]-")));
  CHECK(Has(manifest, "\n0035-cache-vulkan-swapchain-paint-pipeline.linux.patch"));
  CHECK(Has(manifest, "\n0039-lock-free-guest-clock.patch"));

  const std::string doc = ReadFile(kRoot / "docs" / "rexglue-runtime-patch.md");
  CHECK(!std::regex_search(doc, std::regex("\\b003[678]\\b")));
  CHECK(Has(doc, "[0039-lock-free-guest-clock]"));
}

void TestNothingNeedsThePatches() {
  std::printf("no remaining patch needs the removed helpers\n");
  const char* symbols[] = {"host_scale_quarters", "HostScaleQuarters", "GuestToHostX",
                           "GuestToHostY",        "AreHostRenderTargetsReduced", "GetConfigInternalRenderScale",
                           "GetHostViewportInfoAtHostScale", "ScaleScissorToHost", "tmnt_internal_render_scale"};
  int scanned = 0;
  for (const fs::path& patch : FilesUnder("patches/rexglue-sdk", {".patch"})) {
    const std::string text = ReadFile(patch);
    ++scanned;
    for (const char* symbol : symbols) {
      if (Has(text, symbol)) {
        std::printf("  %s uses %s\n", patch.filename().string().c_str(), symbol);
      }
      CHECK(!Has(text, symbol));
    }
  }
  CHECK(scanned > 60);
}

void TestNoReferencesLeft() {
  std::printf("no source, configuration or documentation mentions it\n");
  const char* tokens[] = {"tmnt_internal_render_scale", "internal_render_scale", "InternalRender", "Internal render",
                          "internal render", "ApplyInternalRenderPath", "75% Performance", "50% Low-end",
                          "100% Native", "host_scale_quarters"};
  int scanned = 0;
  for (const fs::path& file : ProjectText()) {
    const std::string text = ReadFile(file);
    ++scanned;
    for (const char* token : tokens) {
      if (Has(text, token)) {
        std::printf("  %s mentions %s\n", file.filename().string().c_str(), token);
      }
      CHECK(!Has(text, token));
    }
  }
  CHECK(scanned > 40);

  const std::string table = ReadFile(kRoot / "src" / "localization" / "pt_br_ui_text.cpp");
  CHECK(Has(table, "Render resolution\tResolu"));
  CHECK(!Has(table, "Native\t"));
  CHECK(!Has(table, "Low-end\t"));
  CHECK(!Has(table, "75% Performance"));
}

void TestLauncherAndRenderPath() {
  std::printf("launcher and render target path\n");
  const std::string launcher = ReadFile(kRoot / "src" / "launcher.cpp");
  CHECK(!Has(launcher, "ui.Combo(\"Internal render resolution\""));
  CHECK(Has(launcher, "ui.Combo(\"Render resolution\""));
  CHECK(Has(launcher, "\"resolution_scale\", \"1\", RenderResolutionOptions("));
  CHECK(Has(launcher, "for (int scale = 1; scale <= 4; ++scale)"));
  CHECK(Has(launcher, "std::to_string(GuestWidth(aspect) * scale) + \"x\" + std::to_string(kGuestHeight * scale)"));
  CHECK(Has(launcher, "ui.Choice(\"Anti-aliasing\""));
  CHECK(Has(launcher, "ui.Toggle(\"Multisampling\""));
  CHECK(Has(launcher, "ui.Choice(\"Texture filtering\""));

  // Nothing sets a render target path; the SDK default and a player's own value stay in charge.
  for (const fs::path& file : FilesUnder("src", {".cpp", ".h"})) {
    const std::string text = ReadFile(file);
    CHECK(!Has(text, "SetFlagByName(\"render_target_path"));
    CHECK(!Has(text, "SetFlagFromCommandLine(\"render_target_path"));
  }
  const std::string settings = ReadFile(kRoot / "src" / "settings.cpp");
  CHECK(Has(settings, "  ApplyAspectRatio();\n  ApplyMultisampling();") ||
        Has(settings, "  ApplyAspectRatio();\r\n  ApplyMultisampling();"));
  CHECK(Has(settings, "      \"resolution_scale\",\n      \"swap_post_effect\",") ||
        Has(settings, "      \"resolution_scale\",\r\n      \"swap_post_effect\","));
}

// The selected Render resolution is the size the game draws at.
void TestRenderResolution() {
  std::printf("Render resolution 1x-4x\n");
  const int widths[] = {1280, 2560, 3840, 5120};
  const int heights[] = {720, 1440, 2160, 2880};
  for (const char* aspect : {"16:9", "21:9", "32:9", "4:3", "garbage", ""}) {
    const int base_width = tmnt::GuestWidth(tmnt::EffectiveAspectRatio(aspect));
    for (int scale = 1; scale <= 4; ++scale) {
      CHECK(base_width * scale == widths[scale - 1]);
      CHECK(tmnt::kGuestHeight * scale == heights[scale - 1]);
    }
  }
  CHECK(tmnt::GuestWidth(tmnt::AspectRatio::k16x9) == 1280 && tmnt::kGuestHeight == 720);
}

}  // namespace

int main() {
  TestFilesAndPatchesGone();
  TestNothingNeedsThePatches();
  TestNoReferencesLeft();
  TestLauncherAndRenderPath();
  TestRenderResolution();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
