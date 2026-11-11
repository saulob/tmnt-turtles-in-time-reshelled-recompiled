// Sidebar icon raster sizes and physical-pixel snapping at common Windows display scales.
// Run: tmnt_launcher_dpi_tests

#include <cmath>
#include <cstdio>

#include "page_icons.h"
#include "trg/layout.h"

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

int IconPixels(float render_scale) {
  return int(trg::layout::kSidebarIcon * render_scale + 0.5f);
}

void TestIconPixelSizes() {
  std::printf("sidebar icons rasterize at the physical pixel size\n");
  CHECK(IconPixels(1.0f) == 21);
  CHECK(IconPixels(1.5f) == 32);
  CHECK(IconPixels(2.25f) == 47);
  for (const float scale : {1.0f, 1.5f, 2.25f}) {
    const float pixels = float(IconPixels(scale));
    const float logical = pixels / scale;
    CHECK(std::fabs(logical * scale - pixels) < 0.0001f);
  }
}

void TestPixelSnapping() {
  std::printf("sidebar origins and text baselines snap to physical pixels\n");
  for (const float scale : {1.0f, 1.5f, 2.25f}) {
    for (const float value : {12.0f, 20.5f, 63.25f, 97.75f}) {
      const float down = trg::layout::SnapDown(value, scale);
      const float up = trg::layout::SnapUp(value, scale);
      const float near = trg::layout::SnapNearest(value, scale);
      CHECK(std::fabs(down * scale - std::floor(down * scale)) < 0.0001f);
      CHECK(std::fabs(up * scale - std::floor(up * scale)) < 0.0001f);
      CHECK(std::fabs(near * scale - std::floor(near * scale)) < 0.0001f);
      CHECK(down <= value && up >= value);
    }
    const float top = trg::layout::CenteredTop(100.5f, 5.25f, scale);
    CHECK(std::fabs(top * scale - std::floor(top * scale)) < 0.0001f);
  }
}

void TestEveryIconRasterizes() {
  std::printf("every sidebar icon rasterizes at 100, 150 and 225 percent\n");
  for (int page = 0; page <= int(tmnt::LauncherPage::kDlc); ++page) {
    for (const int pixels : {21, 32, 47}) {
      const std::vector<uint8_t> rgba = tmnt::RenderPageIcon(tmnt::LauncherPage(page), pixels);
      CHECK(rgba.size() == size_t(pixels) * size_t(pixels) * 4);
      bool opaque = false;
      for (size_t i = 3; i < rgba.size(); i += 4) {
        opaque = opaque || rgba[i] != 0;
      }
      CHECK(opaque);
    }
  }
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestIconPixelSizes();
  TestPixelSnapping();
  TestEveryIconRasterizes();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
