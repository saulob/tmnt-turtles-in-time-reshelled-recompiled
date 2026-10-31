// Local Gamer Card: who a request resolves to, what the card shows, and how B and Escape close it. Pure logic and
// source checks only; the overlay needs the running game.
// Run: tmnt_gamer_card_tests

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "achievement_menu_rules.h"
#include "gamer_card_rules.h"

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

constexpr uint64_t kLocalXuid = 0xB13EBABEBABEBABE;
constexpr uint64_t kRemoteXuid = 0x0009000012345678;

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

GamerCardSources LocalSources() {
  GamerCardSources sources;
  sources.local_xuid = kLocalXuid;
  sources.names[0] = "saulo1";
  sources.names[1] = "Donnie";
  sources.achievements = Summarize(std::vector<uint32_t>{10, 20, 30, 40}, std::vector<bool>{true, false, true, false});
  sources.story_score = 123456;
  return sources;
}

const GamerCardRow* FindRow(const std::vector<GamerCardRow>& rows, const char* label) {
  for (const GamerCardRow& row : rows) {
    if (row.label == label) return &row;
  }
  return nullptr;
}

void TestCurrentProfileOpens() {
  std::printf("gamer card: the current local profile opens\n");
  const GamerCardView view = BuildGamerCardView({.user_index = 0}, LocalSources());
  CHECK(view.available);
  CHECK(view.gamertag == "saulo1");
  CHECK(view.status == "Local profile");
  CHECK(view.game == "Turtles in Time Re-Shelled");
}

void TestLocalXuidOpensTheSameCard() {
  std::printf("gamer card: the local XUID opens the same card\n");
  const GamerCardSources sources = LocalSources();
  const GamerCardView own = BuildGamerCardView({.user_index = 0}, sources);
  const GamerCardView by_xuid = BuildGamerCardView({.user_index = 0, .xuid = kLocalXuid}, sources);
  CHECK(by_xuid.available);
  CHECK(by_xuid.gamertag == own.gamertag);
  CHECK(by_xuid.profile_rows == own.profile_rows);
  CHECK(by_xuid.game_rows == own.game_rows);
}

void TestUnknownXuidIsUnavailable() {
  std::printf("gamer card: an unknown XUID shows an unavailable card and invents nothing\n");
  const GamerCardView view = BuildGamerCardView({.user_index = 0, .xuid = kRemoteXuid}, LocalSources());
  CHECK(!view.available);
  CHECK(view.gamertag.empty());
  CHECK(view.status == "Profile unavailable");
  CHECK(view.profile_rows.empty());
  CHECK(view.game_rows.empty());

  CHECK(!BuildGamerCardView({.user_index = 0, .xuid = kRemoteXuid}, GamerCardSources{}).available);
  CHECK(!BuildGamerCardView({.user_index = 0}, GamerCardSources{}).available);
}

void TestUserIndexBounds() {
  std::printf("gamer card: users outside the four local ones are unavailable\n");
  const GamerCardSources sources = LocalSources();
  CHECK(!BuildGamerCardView({.user_index = 4}, sources).available);
  CHECK(!BuildGamerCardView({.user_index = 0xFF}, sources).available);
  CHECK(!BuildGamerCardView({.user_index = 0xFFFFFFFF}, sources).available);
  CHECK(!BuildGamerCardView({.user_index = 2}, sources).available);
  CHECK(IsLocalGamerCardUser(0) && IsLocalGamerCardUser(3));
  CHECK(!IsLocalGamerCardUser(4) && !IsLocalGamerCardUser(0xFFFFFFFF));
}

void TestAchievementValues() {
  std::printf("gamer card: gamerscore and achievements come from the achievement summary\n");
  const GamerCardView view = BuildGamerCardView({.user_index = 0}, LocalSources());
  const GamerCardRow* gamerscore = FindRow(view.profile_rows, "Gamerscore");
  const GamerCardRow* achievements = FindRow(view.profile_rows, "Achievements");
  CHECK(gamerscore && gamerscore->value == "40 / 100");
  CHECK(achievements && achievements->value == "2 / 4");
}

void TestScoresComeFromTheStats() {
  std::printf("gamer card: scores are shown only when the local stats hold them\n");
  GamerCardSources sources = LocalSources();
  GamerCardView view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(view.game_rows.size() == 1);
  const GamerCardRow* story = FindRow(view.game_rows, "Story Score");
  CHECK(story && story->value == "123456");
  CHECK(!FindRow(view.game_rows, "Survival Score"));

  sources.survival_score = 789;
  view = BuildGamerCardView({.user_index = 0}, sources);
  const GamerCardRow* survival = FindRow(view.game_rows, "Survival Score");
  CHECK(survival && survival->value == "789");

  sources.story_score.reset();
  sources.survival_score.reset();
  view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(view.available);
  CHECK(view.game_rows.empty());
}

void TestEmptyOptionalFieldsAreOmitted() {
  std::printf("gamer card: empty motto, zone and region leave no rows\n");
  GamerCardSources sources = LocalSources();
  GamerCardView view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(view.profile_rows.size() == 2);
  CHECK(!FindRow(view.profile_rows, "Motto"));
  CHECK(!FindRow(view.profile_rows, "Zone"));
  CHECK(!FindRow(view.profile_rows, "Region"));

  sources.zone = 9;
  sources.region = -1;
  view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(view.profile_rows.size() == 2);

  sources.motto = "Cowabunga";
  sources.zone = 2;
  sources.region = 5;
  view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(FindRow(view.profile_rows, "Motto") && FindRow(view.profile_rows, "Motto")->value == "Cowabunga");
  CHECK(FindRow(view.profile_rows, "Zone") && FindRow(view.profile_rows, "Zone")->value == "Pro");
  CHECK(FindRow(view.profile_rows, "Region") && FindRow(view.profile_rows, "Region")->value == "5");
  for (const GamerCardRow& row : view.profile_rows) CHECK(!row.value.empty());
}

void TestMissingDataStillBuilds() {
  std::printf("gamer card: a profile with no achievements or stats still builds\n");
  GamerCardSources sources;
  sources.local_xuid = kLocalXuid;
  sources.names[0] = "saulo1";
  const GamerCardView view = BuildGamerCardView({.user_index = 0}, sources);
  CHECK(view.available);
  CHECK(view.gamertag == "saulo1");
  CHECK(view.profile_rows.empty());
  CHECK(view.game_rows.empty());
}

void TestOtherLocalPlayersShowOnlyTheirName() {
  std::printf("gamer card: players 2-4 show their gamertag, not player 1's progress\n");
  const GamerCardSources sources = LocalSources();
  const GamerCardView view = BuildGamerCardView({.user_index = 1}, sources);
  CHECK(view.available);
  CHECK(view.gamertag == "Donnie");
  CHECK(view.profile_rows.empty());
  CHECK(view.game_rows.empty());
  CHECK(BuildGamerCardView({.user_index = 1, .xuid = kLocalXuid}, sources).available);
  CHECK(!BuildGamerCardView({.user_index = 1, .xuid = kRemoteXuid}, sources).available);
}

void TestOpenCloseState() {
  std::printf("gamer card: B closes once and its press is kept from the game\n");
  AchievementMenuGate gate;
  CHECK(!gate.HoldsInput());
  gate.SetOpen(true);
  CHECK(gate.HoldsInput());
  CHECK(gate.Poll(0, 0, 0).hide_input);

  const auto press = gate.Poll(0, kXinputButtonB, 0);
  CHECK(press.close);
  CHECK(!gate.Poll(0, kXinputButtonB, 0).close);

  gate.SetOpen(false);
  CHECK(gate.HoldsInput());
  CHECK(gate.Poll(0, kXinputButtonB, 0).clear_b);
  CHECK(!gate.Poll(0, 0, 0).clear_b);
  CHECK(!gate.HoldsInput());
}

void TestEscapeClosesOnceInputIsReleased() {
  std::printf("gamer card: Escape and B close only after what was pressed is released\n");
  CloseGate escape;
  CHECK(!escape.Step(false, 0.0));
  escape.Request();
  CHECK(escape.requested());
  CHECK(!escape.Step(true, 1.0));
  CHECK(!escape.Step(true, 1.5));
  CHECK(escape.Step(false, 1.6));

  CloseGate stuck;
  stuck.Request();
  CHECK(!stuck.Step(true, 10.0));
  CHECK(stuck.Step(true, 10.0 + kReleaseTimeoutSeconds));
}

void TestWiring() {
  std::printf("gamer card: the hooks are scoped to the Gamer Card path\n");
  const std::string wrapper = ReadSource("gamer_card.cpp");
  CHECK(Has(wrapper, "REX_HOOK_RAW(sub_826EDB48)"));
  CHECK(Has(wrapper, "__imp__sub_826EDB48(ctx, base)"));
  CHECK(!Has(wrapper, "sub_876AE008"));

  const std::string hooks = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8757E9E0)"));
  CHECK(Has(hooks, "IsLocalGamerCardUser(ctx.r4.u32)"));
  CHECK(Has(hooks, "__imp__sub_8757E9E0(ctx, base)"));

  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::InstallGamerCard"));
  CHECK(Has(app, "new tmnt::GamerCardOverlay"));
}

}  // namespace

int main() {
  TestCurrentProfileOpens();
  TestLocalXuidOpensTheSameCard();
  TestUnknownXuidIsUnavailable();
  TestUserIndexBounds();
  TestAchievementValues();
  TestScoresComeFromTheStats();
  TestEmptyOptionalFieldsAreOmitted();
  TestMissingDataStillBuilds();
  TestOtherLocalPlayersShowOnlyTheirName();
  TestOpenCloseState();
  TestEscapeClosesOnceInputIsReleased();
  TestWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
