// Aspect Ratio: formats, the game-side widths and canvas factor, the setting key and its fallback, the
// retired present_letterbox line, and the launcher, settings and app wiring. Run: tmnt_aspect_ratio_tests

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "aspect_ratio.h"
#include "retired_settings.h"

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

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

void TestFormats() {
  std::printf("formats, labels and ratios\n");
  CHECK(std::size(tmnt::kAspectRatios) == 6);
  const char* keys[] = {"16:9", "16:10", "21:9", "32:9", "4:3", "5:4"};
  const char* labels[] = {"16:9 Original", "16:10", "21:9 Ultrawide", "32:9 Super Ultrawide", "4:3", "5:4"};
  const double ratios[] = {16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 32.0 / 9.0, 4.0 / 3.0, 5.0 / 4.0};
  for (std::size_t i = 0; i < 6; ++i) {
    const tmnt::AspectRatioInfo& info = tmnt::kAspectRatios[i];
    CHECK(std::string(info.key) == keys[i]);
    CHECK(std::string(info.label) == labels[i]);
    CHECK(info.ratio == ratios[i]);
    CHECK(tmnt::InfoFor(info.aspect).aspect == info.aspect);
    CHECK(tmnt::ParseAspectRatio(info.key) == info.aspect);
  }
}

void TestParseFallback() {
  std::printf("setting value parsing and fallback\n");
  CHECK(tmnt::ParseAspectRatio("16:9") == tmnt::AspectRatio::k16x9);
  CHECK(tmnt::ParseAspectRatio("32:9") == tmnt::AspectRatio::k32x9);
  CHECK(tmnt::ParseAspectRatio("") == tmnt::AspectRatio::k16x9);
  CHECK(tmnt::ParseAspectRatio("auto") == tmnt::AspectRatio::k16x9);
  CHECK(tmnt::ParseAspectRatio("21:9 ") == tmnt::AspectRatio::k16x9);
  CHECK(tmnt::ParseAspectRatio("2.33") == tmnt::AspectRatio::k16x9);
}

void TestWindowSizes() {
  std::printf("window sizes\n");
  const tmnt::WindowSize required[] = {
      {1280, 720, tmnt::AspectRatio::k16x9},   {1600, 900, tmnt::AspectRatio::k16x9},
      {1920, 1080, tmnt::AspectRatio::k16x9},  {2560, 1440, tmnt::AspectRatio::k16x9},
      {1280, 800, tmnt::AspectRatio::k16x10},  {1440, 900, tmnt::AspectRatio::k16x10},
      {1260, 540, tmnt::AspectRatio::k21x9},   {1680, 720, tmnt::AspectRatio::k21x9},
      {2100, 900, tmnt::AspectRatio::k21x9},   {1600, 450, tmnt::AspectRatio::k32x9},
      {1920, 540, tmnt::AspectRatio::k32x9},   {1024, 768, tmnt::AspectRatio::k4x3},
      {1280, 960, tmnt::AspectRatio::k4x3},    {1280, 1024, tmnt::AspectRatio::k5x4},
  };
  for (const tmnt::WindowSize& want : required) {
    bool found = false;
    for (const tmnt::WindowSize& size : tmnt::kWindowSizes) {
      found = found || (size.width == want.width && size.height == want.height && size.aspect == want.aspect);
    }
    CHECK(found);
  }
  for (const tmnt::WindowSize& size : tmnt::kWindowSizes) {
    const double ratio = double(size.width) / double(size.height);
    CHECK(std::fabs(ratio - tmnt::InfoFor(size.aspect).ratio) < 0.001);
  }
}

// Game-side widths: 720 lines at every format, with the guest video mode keeping 480 lines.
void TestGameWidths() {
  std::printf("game-side widths, video mode and canvas factor\n");
  const int guest[] = {1280, 1152, 1680, 2560, 960, 900};
  const int video[] = {853, 768, 1120, 1707, 640, 600};
  for (std::size_t i = 0; i < 6; ++i) {
    const tmnt::AspectRatio aspect = tmnt::kAspectRatios[i].aspect;
    const double ratio = tmnt::kAspectRatios[i].ratio;
    CHECK(tmnt::GuestWidth(aspect) == guest[i]);
    CHECK(tmnt::VideoModeWidth(aspect) == video[i]);
    CHECK(std::fabs(double(tmnt::GuestWidth(aspect)) / tmnt::kGuestHeight - ratio) < 1e-3);
    CHECK(std::fabs(double(tmnt::VideoModeWidth(aspect)) / tmnt::kVideoModeHeight - ratio) < 2e-3);
    const float factor = tmnt::CanvasFactor(aspect, float(tmnt::GuestWidth(aspect)), float(tmnt::kGuestHeight));
    CHECK(std::fabs(factor - 1.0f) < 1e-5f);
    CHECK(std::fabs(factor * tmnt::GuestWidth(aspect) - tmnt::kGuestHeight * ratio) < 1e-2);
  }
  CHECK(tmnt::CanvasFactor(tmnt::AspectRatio::k16x9, 1280.0f, 720.0f) == float(16.0 / 9.0) / (1280.0f / 720.0f));
}

void TestRetiredLineMigration() {
  std::printf("retired present_letterbox line\n");
  const std::string keep = "tmnt_show_fps = true\nwindow_width = 1280\n";
  CHECK(tmnt::RemoveSettingLines("tmnt_show_fps = true\npresent_letterbox = false\nwindow_width = 1280\n",
                                "present_letterbox") == keep);
  CHECK(tmnt::RemoveSettingLines("  present_letterbox=true\r\ntmnt_aspect_ratio = \"21:9\"\r\n", "present_letterbox") ==
        "tmnt_aspect_ratio = \"21:9\"\r\n");
  CHECK(tmnt::RemoveSettingLines("window_width = 1\npresent_letterbox = false", "present_letterbox") ==
        "window_width = 1\n");
  CHECK(tmnt::RemoveSettingLines("present_letterbox_extra = 1\n# present_letterbox = false\n", "present_letterbox") ==
        "present_letterbox_extra = 1\n# present_letterbox = false\n");
  CHECK(tmnt::RemoveSettingLines("present_letterbox\n", "present_letterbox") == "present_letterbox\n");
  CHECK(tmnt::RemoveSettingLines(keep, "present_letterbox") == keep);
  CHECK(tmnt::RemoveSettingLines("", "present_letterbox").empty());
}

void TestWiring() {
  std::printf("setting, launcher and app wiring\n");
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_STRING(tmnt_aspect_ratio, \"16:9\""));
  CHECK(Has(settings, "    \"tmnt_aspect_ratio\",\n") || Has(settings, "    \"tmnt_aspect_ratio\",\r\n"));
  CHECK(!Has(settings, "    \"present_letterbox\",\n") && !Has(settings, "    \"present_letterbox\",\r\n"));
  CHECK(Has(settings, "RemoveRetiredPlayerSettings(path);"));
  CHECK(Has(settings, "ApplyAspectRatio();"));
  CHECK(Has(settings, "ParseAspectRatio(rex::cvar::GetFlagByName(\"tmnt_aspect_ratio\"))"));
  CHECK(Has(settings, "SetFlagByName(\"present_letterbox\", \"true\")"));
  CHECK(Has(settings, "SetFlagByName(\"video_mode_width\""));
  CHECK(!Has(settings, "DEBUG_SAULO"));

  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::ApplyAspectRatio();"));

  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "ui.Combo(\"Aspect Ratio\""));
  CHECK(Has(launcher, "\"tmnt_aspect_ratio\", \"16:9\", AspectRatioOptions()"));
  CHECK(Has(launcher, "RenderResolutionOptions(SelectedAspectRatio())"));
  CHECK(!Has(launcher, "Letterbox 16:9"));
  CHECK(!Has(launcher, "\"aspect\""));
  CHECK(!Has(launcher, "\"present_letterbox\""));
}

}  // namespace

int main() {
  TestFormats();
  TestParseFallback();
  TestWindowSizes();
  TestGameWidths();
  TestRetiredLineMigration();
  TestWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
