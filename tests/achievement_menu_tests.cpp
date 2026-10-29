// Achievements menu gate tests: B closes once, the closing press is kept from the game until released, and the
// list scroll follows the controller. Pure logic only; the overlay needs the running game.
// Run: tmnt_achievement_menu_tests

#include <cstdio>

#include "achievement_menu_rules.h"

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

void TestOpenHidesInput() {
  std::printf("achievements gate: open hides input\n");
  AchievementMenuGate gate;
  const auto before = gate.Poll(0, 0, 0);
  CHECK(!before.hide_input);
  CHECK(!before.close);
  CHECK(!gate.HoldsInput());

  gate.SetOpen(true);
  const auto open = gate.Poll(0, 0, 0);
  CHECK(open.hide_input);
  CHECK(!open.close);
  CHECK(gate.HoldsInput());
}

void TestBClosesOnce() {
  std::printf("achievements gate: B closes once\n");
  AchievementMenuGate gate;
  gate.SetOpen(true);
  const auto press = gate.Poll(0, kXinputButtonB, 0);
  CHECK(press.close);
  CHECK(press.hide_input);
  const auto held = gate.Poll(0, kXinputButtonB, 0);
  CHECK(!held.close);
  CHECK(held.hide_input);
}

void TestClosingBPressIsSwallowedUntilReleased() {
  std::printf("achievements gate: closing B is swallowed until released\n");
  AchievementMenuGate gate;
  gate.SetOpen(true);
  CHECK(gate.Poll(0, kXinputButtonB, 0).close);
  gate.SetOpen(false);
  CHECK(gate.HoldsInput());

  const auto still_held = gate.Poll(0, kXinputButtonB, 0);
  CHECK(still_held.clear_b);
  CHECK(!still_held.hide_input);
  CHECK(gate.HoldsInput());

  const auto released = gate.Poll(0, 0, 0);
  CHECK(!released.clear_b);
  CHECK(!gate.HoldsInput());
}

void TestCloseWithoutBReleasesImmediately() {
  std::printf("achievements gate: close without B releases immediately\n");
  AchievementMenuGate gate;
  gate.SetOpen(true);
  gate.Poll(0, 0, 0);
  gate.SetOpen(false);
  CHECK(!gate.HoldsInput());
  const auto after = gate.Poll(0, 0, 0);
  CHECK(!after.clear_b);
}

void TestOnlyTheClosingUsersBIsSwallowed() {
  std::printf("achievements gate: B is tracked per controller\n");
  AchievementMenuGate gate;
  gate.SetOpen(true);
  CHECK(gate.Poll(0, kXinputButtonB, 0).close);
  gate.SetOpen(false);
  CHECK(gate.Poll(1, 0, 0).clear_b);  // user 1 is not pressing B, but B is still held on user 0
  CHECK(gate.HoldsInput());
  CHECK(gate.Poll(0, kXinputButtonB, 0).clear_b);
  CHECK(gate.Poll(0, 0, 0).clear_b == false);
}

void TestScrollDirection() {
  std::printf("achievements gate: scroll direction\n");
  AchievementMenuGate gate;
  gate.SetOpen(true);
  CHECK(gate.Poll(0, kXinputDpadUp, 0).scroll == -1.0f);
  CHECK(gate.Poll(0, kXinputDpadDown, 0).scroll == 1.0f);
  CHECK(gate.Poll(0, 0, kXinputThumbDeadzone).scroll == 0.0f);
  CHECK(gate.Poll(0, 0, kXinputThumbMax).scroll < 0.0f);
  CHECK(gate.Poll(0, 0, -kXinputThumbMax).scroll > 0.0f);
}

}  // namespace

int main() {
  TestOpenHidesInput();
  TestBClosesOnce();
  TestClosingBPressIsSwallowedUntilReleased();
  TestCloseWithoutBReleasesImmediately();
  TestOnlyTheClosingUsersBIsSwallowed();
  TestScrollDirection();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
