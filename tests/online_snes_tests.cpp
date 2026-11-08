// Online SNES gameplay: support is global, each player's choice applies to their own turtle, one locked table per
// gameplay match; Classic Soundtrack stays local; F10 says cheats are unavailable online. Headless: no sockets, no
// threads, no window.
// Run: tmnt_online_snes_tests

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "cheat_menu_rules.h"
#include "classic_soundtrack_notice.h"
#include "online/protocol.h"
#include "online/snes_features.h"
#include "snes_gameplay/online_rules.h"
#include "snes_menu_rules.h"

const char* tmnt::Tr(const char* english) { return english; }

using namespace tmnt::online;

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

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::string(TMNT_SOURCE_DIR) + "/" + relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string source = text.str();
  source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
  return source;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t start = text.find(from);
  if (start == std::string::npos) return {};
  const size_t end = text.find(to, start + from.size());
  return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

constexpr uint32_t kId = 0x1A2B3C4Du;
using Set = SnesGameplaySet;
using Caps = SnesGameplayCapabilities;

Set All() { return Set::FromBits(7); }
Set None() { return {}; }
Set Only(SnesFeature feature) {
  Set set;
  set.Set(feature, true);
  return set;
}

Caps Make(Set on, Set supports = Set::FromBits(7)) {
  Caps caps;
  caps.supports = supports;
  caps.enabled = on;
  caps.compat = {kId, kId, kId};
  return caps.Normalized();
}

SnesMatchTable Table(Set supported, Set a, Set b) {
  SnesMatchTable table;
  table.supported = supported;
  table.players[0] = a;
  table.players[1] = b;
  return table;
}

// What the turtle of `owner` does on the machine of `viewer`: the owner's PC decides it from its own locked seat and the
// game's replication carries the result to everyone else.
Set TurtleUses(const SnesMatchTable& viewer_table, int owner) { return viewer_table.For(owner); }

void TestOnlyThreeGameplayMechanics() {
  CHECK(kSnesFeatureCount == 3 && kSnesSeats == 4);
  CHECK(Only(SnesFeature::kDirectionalCombat).Bits() == 1 && Only(SnesFeature::kThrowControl).Bits() == 2 &&
        Only(SnesFeature::kDash).Bits() == 4);
  CHECK(std::string(SnesKey(SnesFeature::kDirectionalCombat)) == "directional_combat" &&
        std::string(SnesKey(SnesFeature::kThrowControl)) == "throw_control" && std::string(SnesKey(SnesFeature::kDash)) == "dash");
  using tmnt::snes::Feature;
  CHECK(!tmnt::snes::OnlineFeature(Feature::kClassicSoundtrack));
  CHECK(*tmnt::snes::OnlineFeature(Feature::kDash) == SnesFeature::kDash);
  CHECK(!Contains(EncodeMatchHello(5, "00ff00ff00ff00ff", 3, std::string(32, 'a'), 7, Make(All())), "soundtrack"));
  CHECK(!Contains(ReadFile("server/tmnt-lobby/src/protocol.js"), "'classic_soundtrack'"));
}

void TestSupportIsGlobalAndChoicesAreNot() {
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const SnesFeature feature = SnesFeatureAt(i);
    for (const bool a : {false, true}) {
      for (const bool b : {false, true}) {
        const Set mine = a ? Only(feature) : None();
        const Set theirs = b ? Only(feature) : None();
        CHECK(SupportedSnes(Make(mine), Make(theirs)) == All());
      }
    }
    Set without = All();
    without.Set(feature, false);
    CHECK(SupportedSnes(Make(All()), Make(All(), without)) == without);
    CHECK(SupportedSnes(Make(All()), Caps{}) == None());
    Caps other = Make(All());
    other.compat[size_t(i)] = 0x0BADF00Du;
    CHECK(SupportedSnes(Make(All()), other) == without);
  }
}

void TestEveryMachineResolvesTheSamePerPlayerTable() {
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const SnesFeature f = SnesFeatureAt(i);
    for (const bool a : {false, true}) {
      for (const bool b : {false, true}) {
        const SnesMatchTable on_a = Table(All(), a ? Only(f) : None(), b ? Only(f) : None());
        const SnesMatchTable on_b = on_a;
        CHECK(TurtleUses(on_a, 1).Has(f) == a && TurtleUses(on_b, 1).Has(f) == a);
        CHECK(TurtleUses(on_a, 2).Has(f) == b && TurtleUses(on_b, 2).Has(f) == b);
      }
    }
  }
  const SnesMatchTable mixed = Table(All(), Set{true, true, false}, Set{false, true, true});
  CHECK(TurtleUses(mixed, 1) == (Set{true, true, false}) && TurtleUses(mixed, 2) == (Set{false, true, true}));
  const SnesMatchTable no_dash = Table(Set{true, true, false}, All(), All());
  CHECK(TurtleUses(no_dash, 1) == (Set{true, true, false}) && TurtleUses(no_dash, 2) == (Set{true, true, false}));
  CHECK(TurtleUses(mixed, 3) == None() && TurtleUses(mixed, 0) == None() && TurtleUses(mixed, 5) == None());
  CHECK(mixed.AnyOn() && !Table(None(), All(), All()).AnyOn() && !SnesMatchTable{}.AnyOn());
}

void TestStatuses() {
  const SnesFeature f = SnesFeature::kDash;
  const Caps mine = Make(None());
  CHECK(SnesStatusOf(f, Make(All(), None()), Make(All())) == SnesStatus::kUnsupportedLocally);
  CHECK(SnesStatusOf(f, mine, Make(All()), false) == SnesStatus::kUnsupportedByService);
  CHECK(SnesStatusOf(f, mine, std::nullopt) == SnesStatus::kWaiting);
  CHECK(SnesStatusOf(f, mine, Make(All(), Set{true, true, false})) == SnesStatus::kUnsupportedByOther);
  CHECK(SnesStatusOf(f, mine, Make(None())) == SnesStatus::kAvailable);  // their Off is their own turtle's
  Caps different = Make(All());
  different.compat[2] = 5;
  CHECK(SnesStatusOf(f, mine, different) == SnesStatus::kIncompatible);
  different.compat[2] = kSnesCompatMixed;
  CHECK(SnesStatusOf(f, mine, different) == SnesStatus::kIncompatible);
}

void TestTheServiceCanOnlyNarrow() {
  MatchSnes room;
  room.peer = Make(All());
  room.table = Table(All(), All(), All());
  CHECK(VerifiedSupported(Make(None()), room) == All());
  room.peer = Make(All(), Set{true, false, true});
  CHECK(VerifiedSupported(Make(All()), room) == (Set{true, false, true}));
  room.peer = std::nullopt;
  CHECK(VerifiedSupported(Make(All()), room) == None());
}

void TestALockIsUsedWholeOrNotAtAll() {
  MatchSnes room;
  room.peer = Make(All());
  room.locked = SnesMatchLock{1, Table(All(), Set{true, false, false}, Set{false, false, true})};
  const SnesMatchTable used = VerifiedLock(Make(None()), room);
  CHECK(used == room.locked->table && used.For(1) == (Set{true, false, false}) && used.For(2) == (Set{false, false, true}));
  CHECK(VerifiedLock(Make(All(), Set{true, true, false}), room) == SnesMatchTable{});
  Caps other = Make(All());
  other.compat[2] = 77;
  CHECK(VerifiedLock(other, room) == SnesMatchTable{});
  room.locked->table.supported = Set{true, true, false};
  CHECK(VerifiedLock(other, room).For(2) == None() && VerifiedLock(other, room).For(1) == (Set{true, false, false}));
  room.peer = std::nullopt;
  CHECK(VerifiedLock(Make(All()), room) == SnesMatchTable{});
  room.peer = Make(All());
  room.locked = std::nullopt;
  CHECK(VerifiedLock(Make(All()), room) == SnesMatchTable{});

  const Caps a = Make(All(), Set{true, true, false});
  const Caps b = Make(All());
  MatchSnes for_a{b, {}, SnesMatchLock{1, Table(All(), All(), All())}};
  MatchSnes for_b{a, {}, SnesMatchLock{1, Table(All(), All(), All())}};
  CHECK(VerifiedLock(a, for_a) == VerifiedLock(b, for_b) && VerifiedLock(a, for_a) == SnesMatchTable{});
}

void TestWire() {
  Caps caps = Make(Set{true, false, true});
  const std::string hello = EncodeMatchHello(5, "00ff00ff00ff00ff", 3, std::string(32, 'a'), 7, caps);
  CHECK(Contains(hello, "\"snes\":{\"supports\":{\"directional_combat\":true,\"throw_control\":true,\"dash\":true}"));
  CHECK(Contains(hello, "\"enabled\":{\"directional_combat\":true,\"throw_control\":false,\"dash\":true}"));
  CHECK(Contains(EncodeSnesLock(12, 3), "\"op\":\"snes.lock\",\"seq\":12,\"round\":3}"));
  CHECK(Contains(EncodeSnesLock(13, 1, true), "\"op\":\"snes.lock\",\"seq\":13,\"round\":1,\"off\":true}"));

  const auto answer = DecodeServerMessage(
      "{\"re\":4,\"code\":\"ok\",\"capabilities\":{\"snes_features\":true},\"snes\":{\"peer\":{\"supports\":{\"dash\":true}"
      ",\"compat\":{\"dash\":\"mixed\"}},\"supported\":{\"dash\":true,\"throw_control\":true},\"players\":{\"1\":{\"dash\":true},"
      "\"2\":{\"throw_control\":true},\"7\":{\"dash\":true},\"3\":5},\"locked\":{\"round\":2,\"supported\":{\"dash\":true},"
      "\"players\":{\"2\":{\"dash\":true}}}}}");
  CHECK(answer && answer->snes && answer->snes->peer && answer->capabilities->snes_features);
  if (answer && answer->snes && answer->snes->peer) {
    const MatchSnes& snes = *answer->snes;
    CHECK(snes.peer->compat[2] == kSnesCompatMixed);
    CHECK(snes.table.supported == (Set{false, true, true}));
    CHECK(snes.table.For(1) == Only(SnesFeature::kDash) && snes.table.For(2) == Only(SnesFeature::kThrowControl));
    CHECK(!snes.table.players[2] && !snes.table.players[3]);
    CHECK(snes.locked && snes.locked->round == 2 && snes.locked->table.For(2) == Only(SnesFeature::kDash) &&
          snes.locked->table.For(1) == None());
  }
  const auto alone = DecodeServerMessage("{\"re\":4,\"code\":\"ok\",\"snes\":{\"supported\":{},\"players\":{}}}");
  CHECK(alone && alone->snes && !alone->snes->peer && !alone->snes->locked && alone->snes->table == SnesMatchTable{});
  const auto no_round = DecodeServerMessage("{\"ev\":\"match.snes\",\"snes\":{\"locked\":{\"supported\":{\"dash\":true}}}}");
  CHECK(no_round && no_round->snes && !no_round->snes->locked);
  CHECK(!DecodeServerMessage("{\"ev\":\"match.snes\"}"));
  const auto old_worker = DecodeServerMessage("{\"re\":4,\"code\":\"ok\",\"capabilities\":{}}");
  CHECK(old_worker && !old_worker->snes && !old_worker->capabilities->snes_features);
}

void TestKeysMatchTheWorker() {
  const std::string worker = ReadFile("server/tmnt-lobby/src/protocol.js");
  CHECK(Contains(worker, "export const SNES_KEYS = ['directional_combat', 'throw_control', 'dash'];"));
  CHECK(Contains(worker, "snes_features: true"));
}

void TestPalAndNtscShareOnlyDirectionalCombat() {
  using namespace tmnt::snes;
  RuntimeData ntsc;
  ntsc.frame_rate = 60.0988;
  ntsc.facing = {Facing::kRight, Facing::kKeep, Facing::kKeep, Facing::kKeep, Facing::kLeft, Facing::kKeep, Facing::kKeep, Facing::kKeep};
  ntsc.dash_hold_frames = 8;
  ntsc.throw_hold_frames = 5;
  RuntimeData pal = ntsc;
  pal.frame_rate = 50.0070;
  Caps a = Make(All());
  Caps b = Make(None());
  for (const Feature feature : {Feature::kDirectionalCombat, Feature::kThrowControl, Feature::kDash}) {
    a.compat[size_t(*OnlineFeature(feature))] = GameplayCompat(feature, ntsc);
    b.compat[size_t(*OnlineFeature(feature))] = GameplayCompat(feature, pal);
  }
  CHECK(SupportedSnes(a, b) == Only(SnesFeature::kDirectionalCombat));
  CHECK(GameplayCompat(Feature::kClassicSoundtrack, ntsc) == 0);
}

void TestLockCvarCarriesOnlyThisSeat() {
  using namespace tmnt::snes;
  const SnesMatchTable table = Table(All(), Set{true, false, true}, Set{false, true, false});
  CHECK(EncodeOnlineLock(table.For(1)) == (kOnlineLockedFlag | 0b101));
  CHECK(EncodeOnlineLock(table.For(2)) == (kOnlineLockedFlag | 0b010));
  CHECK(EncodeOnlineLock(std::nullopt) == 0);
  CHECK(LockedOn(kOnlineLockedFlag | 4, Feature::kDash) && !LockedOn(4, Feature::kDash));
  CHECK(!LockedOn(kOnlineLockedFlag | 7, Feature::kClassicSoundtrack));
  for (const bool wanted : {false, true}) {
    for (const bool locked_on : {false, true}) {
      CHECK(MechanicActive(wanted, true, false, locked_on) == wanted);
      CHECK(MechanicActive(wanted, true, true, locked_on) == locked_on);
    }
  }
  CHECK(!MechanicActive(true, true, true, LockedOn(0, Feature::kDash)));
}

void TestF9RowsOnline() {
  using namespace tmnt::snes;
  using tmnt::online::SnesStatus;
  OnlineView view;
  view.online = true;
  view.locked = true;
  view.status = {SnesStatus::kAvailable, SnesStatus::kUnsupportedByOther, SnesStatus::kWaiting};
  const RowState soundtrack_on = RowStateOf(view, Feature::kClassicSoundtrack, true, true);
  CHECK(std::string(soundtrack_on.text) == "On" && soundtrack_on.usable && !soundtrack_on.reason);
  CHECK(!RowStateOf(view, Feature::kClassicSoundtrack, false, false).usable);
  const RowState directional = RowStateOf(view, Feature::kDirectionalCombat, true, false);
  CHECK(std::string(directional.text) == "Off" && directional.usable && !directional.reason);
  CHECK(std::string(RowStateOf(view, Feature::kDirectionalCombat, true, true).text) == "On");
  const RowState throws = RowStateOf(view, Feature::kThrowControl, true, true);
  CHECK(std::string(throws.text) == "Unavailable" && !throws.usable &&
        std::string(throws.reason) == "Unavailable: another player does not support this feature");
  const RowState dash = RowStateOf(view, Feature::kDash, true, true);
  CHECK(std::string(dash.text) == "On" && dash.usable && std::string(dash.reason) == "Waiting for players");
  view.status[2] = SnesStatus::kIncompatible;
  CHECK(std::string(RowStateOf(view, Feature::kDash, true, true).reason) == "Incompatible SNES data");
  view.status[2] = SnesStatus::kUnsupportedByService;
  CHECK(!RowStateOf(view, Feature::kDash, true, true).usable);
  CHECK(std::string(OnlineNote(view)) == kMenuNextMatchNote);
  view.locked = false;
  CHECK(std::string(OnlineNote(view)) == kMenuOnlineNote);
  view.online = false;
  CHECK(std::string(RowStateOf(view, Feature::kThrowControl, true, true).text) == "On");
  for (const SnesStatus status : {SnesStatus::kAvailable, SnesStatus::kUnsupportedLocally, SnesStatus::kUnsupportedByOther,
                                  SnesStatus::kUnsupportedByService, SnesStatus::kWaiting, SnesStatus::kIncompatible}) {
    const char* reason = OnlineReason(status);
    CHECK(!reason || !Contains(reason, "opponent"));
  }
}

void TestF10Online() {
  using namespace tmnt::cheats;
  CheatMenuModel menu;
  CHECK(menu.Press(MenuKey::kF10, true, true).action == CheatMenuModel::Action::kUnavailable && !menu.open());
  CHECK(menu.Press(MenuKey::kF10, false, true).action == CheatMenuModel::Action::kUnavailable && !menu.open());
  CHECK(menu.Press(MenuKey::kEnter, true, true).action == CheatMenuModel::Action::kNone);
  CHECK(menu.Press(MenuKey::kF10, true, false).action == CheatMenuModel::Action::kOpen && menu.open());
  CHECK(menu.Press(MenuKey::kEnter, true, true).action == CheatMenuModel::Action::kNone);
  CHECK(menu.Press(MenuKey::kEnter, true, false).action == CheatMenuModel::Action::kChange);
  CHECK(menu.Press(MenuKey::kF10, true, true).action == CheatMenuModel::Action::kClose && !menu.open());
  CHECK(std::string(kCheatsOnlineUnavailable) == "Cheats are unavailable while Online play is enabled.");
  CHECK(std::string(tmnt::SoundtrackNoticeText(tmnt::SoundtrackNotice::kCheatsUnavailable)) == kCheatsOnlineUnavailable);

  const std::string source = ReadFile("src/cheat_menu.cpp");
  const std::string key = Between(source, "void CheatMenu::OnKeyDown(", "\n}\n");
  CHECK(Contains(key, "model_.Press(cheats::MenuKey::kF10, can_open, CheatsBlocked())"));
  CHECK(Contains(key, "if (result.action == cheats::CheatMenuModel::Action::kUnavailable) {\n    REXLOG_INFO(\"Cheats menu blocked: Online play enabled\");\n    ShowCheatsUnavailableNotice();"));
  CHECK(Contains(source, "if (CheatsBlocked()) {\n      RequestClose();\n      return;\n    }"));
  CHECK(Contains(ReadFile("src/cheats_tmnt_r.cpp"), "return Effective(cheat) && cheats::CheatActive(true, InOnlineSession(ctx, base));"));
  CHECK(Contains(ReadFile("src/localization/pt_br_ui_text.cpp"),
                 "Cheats are unavailable while Online play is enabled.\tTrapa\xC3\xA7" "as n\xC3\xA3o est\xC3\xA3o dispon\xC3\xADveis enquanto o jogo online estiver ativado."));
}

void TestTheSoundtrackStaysLocal() {
  using namespace tmnt::snes;
  for (const bool a_on : {false, true}) {
    for (const bool b_on : {false, true}) {
      CHECK(EffectiveFeature(WithOverride(0u, Feature::kClassicSoundtrack, false, a_on), Feature::kClassicSoundtrack, false) == a_on);
      CHECK(EffectiveFeature(WithOverride(0u, Feature::kClassicSoundtrack, false, b_on), Feature::kClassicSoundtrack, false) == b_on);
    }
  }
  const std::string runtime = ReadFile("src/snes_gameplay_runtime.cpp");
  CHECK(Contains(Between(runtime, "LocalSnesCapabilities() {", "\n}\n"), "if (!feature) {\n      continue;"));
  CHECK(!Contains(Between(runtime, "void UpdateSnesOnline() {", "\n}\n"), "Classic"));
  const std::string toggle = Between(runtime, "void ToggleSnesFeatureForSession(", "\n}\n");
  CHECK(Contains(toggle, "if (feature == snes::Feature::kClassicSoundtrack && !SetClassicSoundtrackActive(enabled)) {"));
  CHECK(!Contains(toggle, "Online"));
  for (const char* file : {"src/online/online_service.cpp", "src/online/protocol.cpp", "src/online/snes_features.cpp",
                           "server/tmnt-lobby/src/room.js"}) {
    CHECK(!Contains(ReadFile(file), "classic_soundtrack") && !Contains(ReadFile(file), "ClassicSoundtrack"));
  }
}

void TestHooksDecideOnlyTheOwnTurtleOnline() {
  const std::string hooks = ReadFile("src/snes_gameplay_tmnt_r.cpp");
  CHECK(Contains(hooks, "return snes::MechanicActive(wanted, ready, online, locked_on && own_turtle);"));
  CHECK(Contains(hooks, "!Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)) {\n    return;"));
  CHECK(Contains(hooks, "!Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)) {\n    __imp__sub_8767C730"));
  CHECK(Contains(hooks, "Active(ctx, base, snes::Feature::kDash)") && Contains(hooks, "Active(ctx, base, snes::Feature::kThrowControl)"));
  const std::string runtime = ReadFile("src/snes_gameplay_runtime.cpp");
  CHECK(Contains(runtime, "own = negotiation.locked->For(negotiation.my_slot);"));
  CHECK(Contains(runtime, "PublishLock(snes::EncodeOnlineLock(own));"));
}

void TestNoLocalFallbackCanTurnAMechanicOn() {
  const std::string service = ReadFile("src/online/online_service.cpp");
  CHECK(Contains(Between(service, "void OnlineService::PumpSnes(double now) {", "\n}\n"), "AdoptSnesLock({}, \"timeout\");"));
  CHECK(Contains(Between(service, "void OnlineService::LockSnes() {", "\n}\n"), "AdoptSnesLock({}, \"unsupported\");"));
  CHECK(Contains(Between(service, "void OnlineService::CheckSnesLock() {", "\n}\n"),
                 "AdoptSnesLock(VerifiedLock(LocalSnes(), *match_snes_), \"service\");"));
  size_t adopts = 0;
  for (size_t at = service.find("AdoptSnesLock("); at != std::string::npos; at = service.find("AdoptSnesLock(", at + 1)) ++adopts;
  CHECK(adopts == 4);
  CHECK(Contains(service, "void OnlineService::NoteGameStart() {\n  Locked lock(*this);\n  game_running_ = true;\n  LockSnes();"));
  CHECK(Contains(ReadFile("src/player_match_hook.cpp"), "if (CurrentLive().available) tmnt::Online().NoteGameEnd();"));
}

}  // namespace

int main() {
  std::printf("online SNES gameplay tests\n");
  TestOnlyThreeGameplayMechanics();
  TestSupportIsGlobalAndChoicesAreNot();
  TestEveryMachineResolvesTheSamePerPlayerTable();
  TestStatuses();
  TestTheServiceCanOnlyNarrow();
  TestALockIsUsedWholeOrNotAtAll();
  TestWire();
  TestKeysMatchTheWorker();
  TestPalAndNtscShareOnlyDirectionalCombat();
  TestLockCvarCarriesOnlyThisSeat();
  TestF9RowsOnline();
  TestF10Online();
  TestTheSoundtrackStaysLocal();
  TestHooksDecideOnlyTheOwnTurtleOnline();
  TestNoLocalFallbackCanTurnAMechanicOn();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
