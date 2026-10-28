// Internal render resolution: dimensions for each Render resolution and choice, the split into host and
// resolve scale, the defaults, and where the setting is registered. The render target caches run only with a
// GPU, so ReXGlue's split is mirrored by SplitInternalRenderScale.
// Run: tmnt_internal_render_scale_tests

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "aspect_ratio.h"
#include "internal_render_scale.h"
#include "trg/settings.h"

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

void TestDimensions() {
  std::printf("internal dimensions\n");
  struct Row {
    int scale, percent, width, height;
  };
  const Row rows[] = {
      {1, 100, 1280, 720},  {1, 75, 960, 540},    {1, 50, 640, 360},   {2, 100, 2560, 1440},
      {2, 75, 1920, 1080},  {2, 50, 1280, 720},   {3, 100, 3840, 2160}, {3, 75, 2880, 1620},
      {3, 50, 1920, 1080},  {4, 100, 5120, 2880}, {4, 75, 3840, 2160}, {4, 50, 2560, 1440},
  };
  for (const Row& row : rows) {
    CHECK(tmnt::InternalRenderWidth(row.scale, row.percent) == row.width);
    CHECK(tmnt::InternalRenderHeight(row.scale, row.percent) == row.height);
    const tmnt::InternalRenderSplit split = tmnt::SplitInternalRenderScale(row.scale, row.percent);
    CHECK(tmnt::kPresentedWidth * split.host_quarters / 4 == row.width);
    CHECK(tmnt::kPresentedHeight * split.host_quarters / 4 == row.height);
    // Resolves and textures are never above the render resolution nor below the draw.
    CHECK(split.resolve_scale >= 1 && split.resolve_scale <= row.scale);
    CHECK(split.host_quarters <= split.resolve_scale * 4);
    CHECK(split.host_quarters > (split.resolve_scale - 1) * 4);
    if (row.percent == 100) {
      CHECK(split.host_quarters == row.scale * 4 && split.resolve_scale == row.scale);
    }
  }
  const std::string log = tmnt::DescribeInternalRenderResolution(1, 75);
  CHECK(log == "Internal render resolution: 75% 960x540 -> 1280x720");
  CHECK(tmnt::DescribeInternalRenderResolution(4, 50) == "Internal render resolution: 50% 2560x1440 -> 5120x2880");
}

void TestAspectRatioDimensions() {
  std::printf("internal dimensions per aspect ratio\n");
  struct Row {
    tmnt::AspectRatio aspect;
    int scale, percent, width, height;
  };
  const Row rows[] = {
      {tmnt::AspectRatio::k16x9, 1, 100, 1280, 720}, {tmnt::AspectRatio::k16x9, 1, 75, 960, 540},
      {tmnt::AspectRatio::k16x9, 1, 50, 640, 360},   {tmnt::AspectRatio::k16x10, 1, 75, 864, 540},
      {tmnt::AspectRatio::k4x3, 1, 75, 720, 540},    {tmnt::AspectRatio::k21x9, 1, 50, 840, 360},
      {tmnt::AspectRatio::k21x9, 2, 75, 2520, 1080},
  };
  for (const Row& row : rows) {
    const int base = tmnt::GuestWidth(row.aspect);
    CHECK(tmnt::InternalRenderWidth(row.scale, row.percent, base) == row.width);
    CHECK(tmnt::InternalRenderHeight(row.scale, row.percent) == row.height);
    const tmnt::InternalRenderSplit split = tmnt::SplitInternalRenderScale(row.scale, row.percent);
    CHECK(base * split.host_quarters / 4 == row.width);
    CHECK(tmnt::kGuestHeight * split.host_quarters / 4 == row.height);
  }
  CHECK(tmnt::DescribeInternalRenderResolution(1, 75, tmnt::GuestWidth(tmnt::AspectRatio::k4x3)) ==
        "Internal render resolution: 75% 720x540 -> 960x720");
  CHECK(tmnt::DescribeInternalRenderResolution(1, 75) == "Internal render resolution: 75% 960x540 -> 1280x720");
}

void TestDefaults() {
  std::printf("defaults\n");
  CHECK(tmnt::NormalizeInternalRenderScale(100) == 100);
  CHECK(tmnt::NormalizeInternalRenderScale(75) == 75);
  CHECK(tmnt::NormalizeInternalRenderScale(50) == 50);
  // An unset or unknown value draws at 100%.
  for (int value : {0, -1, 1, 25, 60, 90, 101, 200}) {
    CHECK(tmnt::NormalizeInternalRenderScale(value) == 100);
  }
  CHECK(tmnt::InternalRenderWidth(2, 0) == 2560);
}

void TestRenderTargetPath() {
  std::printf("render target path\n");
  CHECK(!tmnt::RequiredRenderTargetPath(100, ""));
  CHECK(tmnt::RequiredRenderTargetPath(75, "") == "rtv");
  CHECK(tmnt::RequiredRenderTargetPath(50, "") == "rtv");
  // Settings without the key, or an unknown value, are 100%.
  CHECK(!tmnt::RequiredRenderTargetPath(0, ""));
  CHECK(!tmnt::RequiredRenderTargetPath(60, ""));
  // A path the player chose is kept.
  CHECK(!tmnt::RequiredRenderTargetPath(75, "rov"));
  CHECK(!tmnt::RequiredRenderTargetPath(50, "rtv"));
}

void TestPersistence() {
  std::printf("save and reload\n");
  const std::filesystem::path file = std::filesystem::temp_directory_path() / "tmnt_internal_render_scale_test.toml";
  std::filesystem::remove(file);
  {
    std::ofstream(file) << "# Turtles in Time Re-Shelled player settings\nresolution_scale = 2\n";
  }
  {
    trg::TextFileSettings settings(file.string());
    // An existing file without the setting reads as the 100% default.
    CHECK(!settings.Has("tmnt_internal_render_scale"));
    CHECK(tmnt::NormalizeInternalRenderScale(settings.GetInt("tmnt_internal_render_scale", 100)) == 100);
    settings.Set("tmnt_internal_render_scale", "75");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.GetInt("tmnt_internal_render_scale", 100) == 75);
    CHECK(settings.GetInt("resolution_scale", 1) == 2);
    settings.Set("tmnt_internal_render_scale", "50");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.GetInt("tmnt_internal_render_scale", 100) == 50);
    settings.ResetToDefaults();
    CHECK(settings.GetInt("tmnt_internal_render_scale", 100) == 100);
  }
  std::filesystem::remove(file);
}

void TestRegistration() {
  std::printf("setting, launcher row and runtime patch\n");
  const std::string src = TMNT_SRC_DIR;
  const std::string settings = ReadFile(src + "/settings.cpp");
  CHECK(Has(settings, "      \"resolution_scale\",\n      \"tmnt_internal_render_scale\",\n") ||
        Has(settings, "      \"resolution_scale\",\r\n      \"tmnt_internal_render_scale\",\r\n"));

  const std::string launcher = ReadFile(src + "/launcher.cpp");
  const size_t render_resolution = launcher.find("ui.Combo(\"Render resolution\"");
  const size_t internal = launcher.find("ui.Combo(\"Internal render resolution\"");
  const size_t anti_aliasing = launcher.find("ui.Choice(\"Anti-aliasing\"");
  CHECK(render_resolution != std::string::npos && internal != std::string::npos &&
        anti_aliasing != std::string::npos);
  CHECK(render_resolution < internal && internal < anti_aliasing);
  CHECK(Has(launcher, "\"tmnt_internal_render_scale\", \"100\""));
  CHECK(Has(launcher, "{\"100\", \"100% Native\"}, {\"75\", \"75% Performance\"}, {\"50\", \"50% Low-end\"}"));

  const std::string app = ReadFile(src + "/tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(settings, "  ApplyInternalRenderPath();\n  ApplyMultisampling();") ||
        Has(settings, "  ApplyInternalRenderPath();\r\n  ApplyMultisampling();"));
  CHECK(Has(settings, "  ApplyAspectRatio();\n  ApplyInternalRenderPath();") ||
        Has(settings, "  ApplyAspectRatio();\r\n  ApplyInternalRenderPath();"));
  CHECK(Has(app, "tmnt::ApplyInternalRenderPath();\n      return defaults;") ||
        Has(app, "tmnt::ApplyInternalRenderPath();\r\n      return defaults;"));

  const std::string patch = ReadFile(src + "/../patches/rexglue-sdk/0036-internal-render-scale.patch");
  CHECK(Has(patch, "REXCVAR_DEFINE_INT32(tmnt_internal_render_scale, 100,"));
}

}  // namespace

int main() {
  TestDimensions();
  TestAspectRatioDimensions();
  TestDefaults();
  TestRenderTargetPath();
  TestPersistence();
  TestRegistration();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
