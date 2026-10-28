// Frame counter tests: the text, when it is drawn, its wiring and the launcher setting. Pure logic and source
// checks only; the overlay and the F2 toggle need the running game.
// Run: tmnt_frame_counter_tests

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "frame_counter.h"

using namespace tmnt;

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

void TestText() {
  std::printf("frame counter text\n");
  CHECK(FrameCounterText({60.0, 1000.0 / 60.0, 60}) == "60 FPS  16.7 ms");
  CHECK(FrameCounterText({120.0, 1000.0 / 120.0, 120}) == "120 FPS  8.3 ms");
  CHECK(FrameCounterText({59.94, 16.6833, 60}) == "60 FPS  16.7 ms");
}

void TestVisibility() {
  std::printf("frame counter visibility\n");
  const GuestFrameStats measured{60.0, 1000.0 / 60.0, 120};
  CHECK(!FrameCounterVisible(false, measured));
  CHECK(!FrameCounterVisible(true, GuestFrameStats{}));
  CHECK(FrameCounterVisible(true, measured));
}

void TestWiring() {
  std::printf("frame counter wiring\n");
  const std::string counter = ReadSource("frame_counter.cpp");
  CHECK(Has(counter, "RegisterBind(\"bind_tmnt_frame_counter\", \"F2\""));
  CHECK(Has(counter, "GetForegroundDrawList"));
  CHECK(Has(counter, "GetGuestFrameStats()"));

  const std::string host = ReadSource("window_host.cpp");
  CHECK(Has(host, "g_stats = {fps,"));  // the title and the counter share one sample
  CHECK(Has(host, "tmnt_show_fps"));

  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_show_frame_counter, false"));
  CHECK(Has(settings, "\"tmnt_show_frame_counter\","));  // saved and reset with the other game settings

  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "\"Frame rate in title\""));
  CHECK(Has(launcher, "\"tmnt_show_frame_counter\", false, \"Hidden\", \"Shown\""));

  const std::string overlay = ReadSource("achievements_overlay.cpp");
  CHECK(Has(overlay, "X##close_achievements"));
  CHECK(Has(overlay, "\"B  Back\""));
}

}  // namespace

int main() {
  TestText();
  TestVisibility();
  TestWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
