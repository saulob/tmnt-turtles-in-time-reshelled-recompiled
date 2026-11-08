// SNES menu: the F9 model, the session overrides of the four SNES features, availability by import state, the
// switched-off reset, the retired master setting, the launcher page and the wiring. Headless; the real-ROM checks run
// only when game/snes holds the ROMs.
// Run: tmnt_snes_menu_tests

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cheat_menu_rules.h"
#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack_runtime.h"
#include "retired_settings.h"
#include "snes_gameplay/online_rules.h"
#include "snes_menu_rules.h"

namespace fs = std::filesystem;
using namespace tmnt;
using namespace tmnt::snes;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                 \
  } while (0)

const fs::path kRoot = TMNT_ROOT_DIR;

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string Source(const char* relative) { return ReadText(kRoot / relative); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  const size_t b = a == std::string::npos ? std::string::npos : text.find(to, a + from.size());
  return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
}

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / (std::string("tmnt_snes_menu_") + name);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

constexpr Availability kAllReady{true, GameplayStatus::kReady};
constexpr Availability kNoRom{false, GameplayStatus::kNotImported};
constexpr Availability kOldImport{true, GameplayStatus::kReimportRequired};

using Action = MenuModel::Action;

void TestFeatures() {
  std::printf("features: four, audio first, each with its own saved setting and no master\n");
  CHECK(kFeatureCount == 4 && kFeatures.size() == 4 && kMenuRowCount == 4);
  const char* labels[] = {"Classic Soundtrack", "Directional Combat", "SNES Throw Control", "SNES Dash"};
  const char* settings[] = {"tmnt_classic_soundtrack", "tmnt_snes_directional_combat", "tmnt_snes_throw_control",
                            "tmnt_snes_dash"};
  for (size_t i = 0; i < kFeatureCount; ++i) {
    CHECK(std::string(MenuRow(i).label) == labels[i]);
    CHECK(std::string(MenuRow(i).setting) == settings[i]);
    CHECK(static_cast<size_t>(MenuRow(i).feature) == i);
    CHECK(MenuRow(i).gameplay == (i != 0));
  }
  CHECK(std::string(kFeatures[0].setting) == kClassicSoundtrackSetting);
  CHECK(kMenuAudioRow == 0 && kMenuGameplayRow == 1);
  CHECK(std::string(kSessionSetting) == "tmnt_snes_session");
  CHECK(std::string(kMenuTitle) == "SNES" && std::string(kMenuAudioSection) == "AUDIO" &&
        std::string(kMenuGameplaySection) == "GAMEPLAY" && std::string(kMenuCloseHint) == "F9 Close" &&
        std::string(kMenuNote) == "Not saved to the launcher" &&
        std::string(kMenuOnlineNote) == "Online: each player's gameplay choices apply to their own turtle" &&
        std::string(kMenuNextMatchNote) == "Applies to the next online match");
}

void TestSessionOverrides() {
  std::printf("session: saved value, optional override, effective value, for each feature\n");
  for (const FeatureInfo& info : kFeatures) {
    for (const bool saved : {false, true}) {
      CHECK(EffectiveFeature(0u, info.feature, saved) == saved);
      CHECK(OverrideOf(0u, info.feature) == cheats::Override::kFollowsSaved);

      uint32_t session = ToggledOverride(0u, info.feature, saved);
      CHECK(EffectiveFeature(session, info.feature, saved) == !saved);
      CHECK(OverrideOf(session, info.feature) == (saved ? cheats::Override::kOff : cheats::Override::kOn));
      session = ToggledOverride(session, info.feature, saved);
      CHECK(session == 0u && EffectiveFeature(session, info.feature, saved) == saved);
    }
  }

  uint32_t session = ToggledOverride(0u, Feature::kDash, true);
  CHECK(!EffectiveFeature(session, Feature::kDash, true));
  CHECK(EffectiveFeature(session, Feature::kDirectionalCombat, true));
  CHECK(EffectiveFeature(session, Feature::kThrowControl, true));
  CHECK(!EffectiveFeature(session, Feature::kClassicSoundtrack, false));
  session = ToggledOverride(session, Feature::kClassicSoundtrack, false);
  CHECK(EffectiveFeature(session, Feature::kClassicSoundtrack, false) && !EffectiveFeature(session, Feature::kDash, true));
  CHECK(session < (1u << (kFeatureCount * 2)));
  CHECK(WithOverride(0u, Feature::kThrowControl, true, true) == 0u);

  // Closing and reopening the menu does not touch the overrides, and a new process has none.
  MenuModel menu;
  menu.Press(MenuKey::kF9, true, kAllReady);
  menu.Press(MenuKey::kDown, true, kAllReady);
  menu.Press(MenuKey::kEnter, true, kAllReady);
  const uint32_t before = session;
  menu.Press(MenuKey::kF9, true, kAllReady);
  menu.Press(MenuKey::kF9, true, kAllReady);
  CHECK(session == before && !EffectiveFeature(session, Feature::kDash, true));
  CHECK(EffectiveFeature(0u, Feature::kDash, true) && EffectiveFeature(0u, Feature::kDirectionalCombat, true));

  const std::string runtime = Source("src/snes_gameplay_runtime.cpp");
  CHECK(Has(runtime, "REXCVAR_DEFINE_UINT32(tmnt_snes_session, 0,"));
  CHECK(Has(runtime, "rex::cvar::SetFlagByName(snes::kSessionSetting, std::to_string(next));"));
  CHECK(Has(runtime, "snes::EffectiveFeature(REXCVAR_GET(tmnt_snes_session), feature, SavedValue(feature))"));
  CHECK(Has(Source("src/snes_gameplay_tmnt_r.cpp"), "rex::cvar::Query<uint32_t>(snes::kSessionSetting)"));
}

void TestSessionNeverSaves() {
  std::printf("session: the F9 menu never writes settings.toml or a saved value\n");
  const std::string menu = Source("src/snes_menu.cpp") + Source("src/snes_menu.h") + Source("src/snes_gameplay_runtime.cpp");
  for (const char* write : {"Save(", "SaveNow(", "settings_", "TextFileSettings", "settings.toml", "SaveConfig"}) {
    CHECK(!Has(menu, write));
  }
  const std::string runtime = Source("src/snes_gameplay_runtime.cpp");
  CHECK(Count(runtime, "SetFlagByName(") == 3);
  CHECK(Count(runtime, "SetFlagByName(snes::kSessionSetting") == 1 && Count(runtime, "SetFlagByName(snes::kRuntimeDataSetting") == 1 &&
        Count(runtime, "SetFlagByName(snes::kOnlineLockSetting") == 1);
  for (const char* key : {"kDirectionalCombatSetting", "kThrowControlSetting", "kDashSetting", "kClassicSoundtrackSetting"}) {
    CHECK(!Has(runtime, std::string("SetFlagByName(") + key));
  }
}

void TestMenuModel() {
  std::printf("F9: open, close, Escape, B, navigation, change, exactly four rows\n");
  MenuModel menu;
  CHECK(!menu.open());
  for (const MenuKey key : {MenuKey::kEnter, MenuKey::kPadA, MenuKey::kEscape, MenuKey::kPadB, MenuKey::kUp, MenuKey::kDown}) {
    CHECK(menu.Press(key, true, kAllReady).action == Action::kNone && !menu.open() && menu.selected() == 0);
  }

  CHECK(menu.Press(MenuKey::kF9, false, kAllReady).action == Action::kNone && !menu.open());
  CHECK(menu.Press(MenuKey::kF9, true, kAllReady).action == Action::kOpen && menu.open());
  CHECK(menu.Press(MenuKey::kF9, true, kAllReady).action == Action::kClose && !menu.open());
  for (const MenuKey close : {MenuKey::kEscape, MenuKey::kPadB}) {
    menu.Press(MenuKey::kF9, true, kAllReady);
    CHECK(menu.Press(close, true, kAllReady).action == Action::kClose && !menu.open());
  }

  menu.Press(MenuKey::kF9, true, kAllReady);
  CHECK(menu.selected() == 0);
  for (size_t expected = 1; expected <= kMenuRowCount; ++expected) {
    CHECK(menu.Press(MenuKey::kDown, true, kAllReady).action == Action::kNone);
    CHECK(menu.selected() == expected % kMenuRowCount);
  }
  menu.Press(MenuKey::kUp, true, kAllReady);
  CHECK(menu.selected() == 3);
  menu.Press(MenuKey::kUp, true, kAllReady);
  CHECK(menu.selected() == 2);

  for (const MenuKey change : {MenuKey::kEnter, MenuKey::kPadA}) {
    const MenuModel::Result result = menu.Press(change, true, kAllReady);
    CHECK(result.action == Action::kChange && result.row == 2 && menu.open() && menu.selected() == 2);
  }
  menu.Select(1);
  CHECK(menu.Press(MenuKey::kEnter, true, kAllReady).row == 1);
  menu.Select(9);
  CHECK(menu.selected() == 1);

  // Opening again starts at the first row, and a dialog that went away by itself leaves the menu closed.
  menu.Press(MenuKey::kEscape, true, kAllReady);
  menu.Press(MenuKey::kF9, true, kAllReady);
  CHECK(menu.selected() == 0);
  menu.Closed();
  CHECK(!menu.open());
  menu.Select(2);
  CHECK(menu.selected() == 0);
}

void TestF9OnlyOpensAndCloses() {
  std::printf("old F9: pressing F9 alone changes no feature\n");
  MenuModel menu;
  for (int press = 0; press < 6; ++press) {
    const MenuModel::Result result = menu.Press(MenuKey::kF9, true, kAllReady);
    CHECK(result.action == (press % 2 == 0 ? Action::kOpen : Action::kClose));
    CHECK(result.action != Action::kChange);
  }
  for (const Availability& available : {kAllReady, kNoRom, kOldImport}) {
    MenuModel fresh;
    CHECK(fresh.Press(MenuKey::kF9, true, available).action == Action::kOpen);
    CHECK(fresh.Press(MenuKey::kF9, true, available).action == Action::kClose);
  }

  const std::string menu_source = Source("src/snes_menu.cpp");
  const size_t key = menu_source.find("void SnesMenu::OnKeyDown(");
  CHECK(key != std::string::npos);
  const std::string on_key = menu_source.substr(key, menu_source.find("void SnesMenu::DialogClosed") - key);
  CHECK(!Has(on_key, "ToggleSnesFeatureForSession") && !Has(on_key, "SetClassicSoundtrackActive") && !Has(on_key, "Toggle"));
  CHECK(Has(on_key, "e.virtual_key() != rex::ui::VirtualKey::kF9") && Has(on_key, "e.set_handled(true);"));

  const std::string overlay = Source("src/classic_soundtrack_overlay.cpp") + Source("src/classic_soundtrack_overlay.h");
  for (const char* gone : {"RegisterBind", "kClassicSoundtrackKey", "SwitchSoundtrack", "SetClassicSoundtrackActive",
                           "ToggleClassicSoundtrack", "bind_tmnt_classic_soundtrack"}) {
    CHECK(!Has(overlay, gone));
  }
  CHECK(!Has(Source("src/classic_soundtrack_runtime.cpp") + Source("src/classic_soundtrack_runtime.h"), "Toggle"));
  CHECK(!Has(Source("src/classic_soundtrack/activation.h"), "Toggle"));

  size_t f9_binds = 0;
  for (const auto& entry : fs::directory_iterator(kRoot / "src")) {
    if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
    const std::string text = ReadText(entry.path());
    for (size_t at = text.find("RegisterBind("); at != std::string::npos; at = text.find("RegisterBind(", at + 1)) {
      const std::string call = text.substr(at, 160);
      f9_binds += Has(call, "\"F9\"") ? 1 : 0;
    }
  }
  CHECK(f9_binds == 0);
}

void TestDisabledRows() {
  std::printf("availability: no ROM disables every row, an old import keeps the soundtrack, Ready enables all\n");
  for (const Feature feature : {Feature::kClassicSoundtrack, Feature::kDirectionalCombat, Feature::kThrowControl, Feature::kDash}) {
    CHECK(RowAvailable(kAllReady, feature));
    CHECK(!RowAvailable(kNoRom, feature));
    CHECK(RowAvailable(kOldImport, feature) == (feature == Feature::kClassicSoundtrack));
  }
  CHECK(StatusMessage(kAllReady) == nullptr);
  CHECK(std::string(StatusMessage(kNoRom)) == "SNES ROM not imported");
  CHECK(std::string(StatusMessage(kOldImport)) ==
        "SNES gameplay data unavailable: re-import the ROM in the launcher");
  CHECK(std::string(StatusMessage({true, GameplayStatus::kUnsupportedRom})) ==
        "SNES gameplay data unavailable: unsupported ROM version");
  CHECK(std::string(StatusMessage({false, GameplayStatus::kReady})) == "SNES soundtrack: Unavailable");
  CHECK(std::string(StatusMessage({false, GameplayStatus::kReimportRequired})) ==
        "SNES gameplay data unavailable: re-import the ROM in the launcher");

  // No ROM: the menu opens, nothing changes, and it still closes with F9, Escape and B.
  MenuModel menu;
  CHECK(menu.Press(MenuKey::kF9, true, kNoRom).action == Action::kOpen);
  for (size_t row = 0; row < kMenuRowCount; ++row) {
    menu.Select(row);
    CHECK(menu.Press(MenuKey::kEnter, true, kNoRom).action == Action::kNone);
    CHECK(menu.Press(MenuKey::kPadA, true, kNoRom).action == Action::kNone);
  }
  CHECK(menu.Press(MenuKey::kDown, true, kNoRom).action == Action::kNone);
  CHECK(menu.Press(MenuKey::kF9, true, kNoRom).action == Action::kClose);
  for (const MenuKey close : {MenuKey::kEscape, MenuKey::kPadB}) {
    menu.Press(MenuKey::kF9, true, kNoRom);
    CHECK(menu.Press(close, true, kNoRom).action == Action::kClose && !menu.open());
  }

  // Old import: only the soundtrack row changes.
  menu.Press(MenuKey::kF9, true, kOldImport);
  CHECK(menu.Press(MenuKey::kEnter, true, kOldImport).action == Action::kChange);
  for (size_t row = 1; row < kMenuRowCount; ++row) {
    menu.Select(row);
    CHECK(menu.Press(MenuKey::kEnter, true, kOldImport).action == Action::kNone);
  }

  const std::string runtime = Source("src/snes_gameplay_runtime.cpp");
  CHECK(Has(runtime, "if (!snes::RowAvailable(SnesAvailability(), feature)) {"));
  CHECK(Has(runtime, "snes::Availability SnesAvailability() { return {IsClassicSoundtrackReady(), g_gameplay_status}; }"));
  CHECK(Has(runtime, "g_gameplay_status = status.status;"));
  CHECK(Has(Source("src/snes_menu.cpp"), "snes::RowAvailable(available_, row.feature)"));
  CHECK(Has(Source("src/snes_menu.cpp"), "snes::StatusMessage(available_)"));
}

void WriteManifest(const fs::path& root, uint32_t importer_version) {
  const fs::path folder = classic::SoundtrackFolder(root);
  classic::ClassicSoundtrackManifest manifest;
  CHECK(classic::ParseManifest(ReadText(folder / classic::kManifestFile), manifest));
  manifest.importer_version = importer_version;
  std::ofstream(folder / classic::kManifestFile, std::ios::binary) << classic::SerializeManifest(manifest);
}

Availability FromImport(const fs::path& root) {
  const GameplayStatusResult gameplay = QueryGameplayStatus(root);
  return {classic::QuerySoundtrackStatus(root).status == classic::SoundtrackStatus::kReady, gameplay.status};
}

void TestAvailabilityFromImports() {
  std::printf("availability: no import, v3 Ready, old v2 import, remove\n");
  TempDir dir("imports");
  Availability none = FromImport(dir.path);
  CHECK(!none.audio && none.gameplay == GameplayStatus::kNotImported);
  CHECK(!RowAvailable(none, Feature::kClassicSoundtrack) && !RowAvailable(none, Feature::kDash));
  CHECK(std::string(StatusMessage(none)) == "SNES ROM not imported");

  const char* files[] = {"Teenage Mutant Ninja Turtles IV - Turtles in Time (USA).sfc",
                         "Teenage Mutant Hero Turtles IV - Turtles in Time (Europe).sfc",
                         "Teenage Mutant Ninja Turtles IV - Turtles in Time (Australia).sfc"};
  fs::path rom;
  for (const char* file : files) {
    if (fs::exists(kRoot / "game" / "snes" / file)) {
      rom = kRoot / "game" / "snes" / file;
      break;
    }
  }
  if (rom.empty()) {
    std::printf("    no SNES ROMs in game/snes: import checks skipped\n");
    return;
  }
  CHECK(classic::ImportSoundtrack(rom, dir.path).ok);
  const Availability ready = FromImport(dir.path);
  CHECK(ready.audio && ready.gameplay == GameplayStatus::kReady && StatusMessage(ready) == nullptr);
  for (const FeatureInfo& info : kFeatures) CHECK(RowAvailable(ready, info.feature));

  fs::remove(classic::SoundtrackFolder(dir.path) / kGameplayFile);
  WriteManifest(dir.path, 2);
  const Availability old = FromImport(dir.path);
  CHECK(old.audio && old.gameplay == GameplayStatus::kReimportRequired);
  CHECK(RowAvailable(old, Feature::kClassicSoundtrack) && !RowAvailable(old, Feature::kDirectionalCombat) &&
        !RowAvailable(old, Feature::kThrowControl) && !RowAvailable(old, Feature::kDash));
  CHECK(std::string(StatusMessage(old)) == "SNES gameplay data unavailable: re-import the ROM in the launcher");

  CHECK(classic::ImportSoundtrack(rom, dir.path).ok);
  std::string error;
  CHECK(classic::RemoveSoundtrack(dir.path, error));
  const Availability removed = FromImport(dir.path);
  CHECK(!removed.audio && removed.gameplay == GameplayStatus::kNotImported);
  for (const FeatureInfo& info : kFeatures) CHECK(!RowAvailable(removed, info.feature));
}

void TestGameplayEffect() {
  std::printf("gameplay: offline the effective value AND data Ready, online only the locked set AND data Ready\n");
  for (const bool wanted : {false, true}) {
    for (const bool ready : {false, true}) {
      for (const bool online : {false, true}) {
        for (const bool locked_on : {false, true}) {
          CHECK(MechanicActive(wanted, ready, online, locked_on) == (ready && (online ? locked_on : wanted)));
        }
      }
    }
  }
  for (const FeatureInfo& info : kFeatures) {
    if (!info.gameplay) continue;
    const uint32_t off = ToggledOverride(0u, info.feature, true);
    const uint32_t on = ToggledOverride(0u, info.feature, false);
    CHECK(MechanicActive(EffectiveFeature(off, info.feature, true), true, false, false) == false);
    CHECK(MechanicActive(EffectiveFeature(on, info.feature, false), true, false, false) == true);
    CHECK(MechanicActive(EffectiveFeature(on, info.feature, false), false, false, false) == false);
    CHECK(MechanicActive(EffectiveFeature(on, info.feature, false), true, true, false) == false);
    CHECK(MechanicActive(EffectiveFeature(off, info.feature, true), true, true, true) == true);
    CHECK(MechanicActive(EffectiveFeature(0u, info.feature, true), true, false, false) == true);
  }

  const std::string hooks = Source("src/snes_gameplay_tmnt_r.cpp");
  CHECK(Has(hooks, "const bool online = (wanted || locked_on) && ready && Online(ctx, base);") &&
        Has(hooks, "return snes::MechanicActive(wanted, ready, online, locked_on && own_turtle);"));
  CHECK(Has(hooks, "Active(ctx, base, snes::Feature::kDash)") && Has(hooks, "Active(ctx, base, snes::Feature::kThrowControl)") &&
        Has(hooks, "Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)"));
  CHECK(!Has(hooks, "kSnesGameplaySetting") && !Has(hooks, "tmnt_snes_gameplay\""));
  CHECK(Has(hooks, "snes::EffectiveFeature(session, feature, rex::cvar::Query<bool>(snes::InfoOf(feature).setting))"));
}

void TestSwitchedOffReset() {
  std::printf("gameplay: switching a feature off drops its transient state, nothing else\n");
  RuntimeData data;
  data.frame_rate = 60.0;
  data.facing = {Facing::kRight, Facing::kRight, Facing::kKeep, Facing::kLeft,
                 Facing::kLeft,  Facing::kLeft,  Facing::kKeep, Facing::kRight};
  data.dash_hold_frames = 32;
  data.throw_hold_frames = 6;

  const uint32_t all_on = FeatureBit(Feature::kDirectionalCombat) | FeatureBit(Feature::kThrowControl) | FeatureBit(Feature::kDash);
  EffectiveChanges changes;
  CHECK(changes.Observe(all_on) == 0);
  CHECK(changes.Observe(all_on) == 0);

  StickState stick;
  for (int frame = 0; frame < 40; ++frame) PushStick(stick, 0, 1.0 / 60.0, data);
  CHECK(stick.dashing && stick.same_held > 0.5 && stick.held > 0.5 && stick.previous == 0);

  const uint32_t dash_off = all_on & ~FeatureBit(Feature::kDash);
  const uint32_t changed = changes.Observe(dash_off);
  CHECK(changed == FeatureBit(Feature::kDash));
  ResetSwitchedOff(stick, changed, dash_off);
  CHECK(!stick.dashing && stick.same_held == 0.0);
  CHECK(stick.held > 0.5 && stick.previous == 0 && stick.direction == 0);
  CHECK(changes.Observe(dash_off) == 0);

  const uint32_t throw_off = dash_off & ~FeatureBit(Feature::kThrowControl);
  const uint32_t throw_changed = changes.Observe(throw_off);
  CHECK(throw_changed == FeatureBit(Feature::kThrowControl));
  ResetSwitchedOff(stick, throw_changed, throw_off);
  CHECK(stick.held == 0.0 && stick.previous == kNoDirection && stick.direction == 0);
  CHECK(ChooseGrab(stick, kRight, data) == GrabChoice::kSlam);

  StickState untouched;
  HoldStick(untouched, 0, 0.5);
  const StickState copy = untouched;
  const uint32_t combat_off = throw_off & ~FeatureBit(Feature::kDirectionalCombat);
  ResetSwitchedOff(untouched, changes.Observe(combat_off), combat_off);
  CHECK(untouched.held == copy.held && untouched.same_held == copy.same_held && untouched.direction == copy.direction);

  // Switching on, or leaving a feature on, resets nothing.
  StickState running;
  for (int frame = 0; frame < 40; ++frame) PushStick(running, 0, 1.0 / 60.0, data);
  ResetSwitchedOff(running, changes.Observe(all_on), all_on);
  CHECK(running.dashing && running.held > 0.5);

  StickState released;
  ResetDashState(released);
  ResetThrowState(released);
  CHECK(released.direction == kNoDirection && released.held == 0.0);

  const std::string hooks = Source("src/snes_gameplay_tmnt_r.cpp");
  const std::string controller_of = Between(hooks, "Controller* ControllerOf(", "const Controller* ControllerFor");
  CHECK(Has(controller_of, "DropSwitchedOffState(ctx, base);"));
  CHECK(Has(hooks, "snes::ResetSwitchedOff(controller.stick, changed, effective);"));
}

void TestOnline() {
  std::printf("online: gameplay rows are the player's own choice while the match supports them, Classic Soundtrack stays local\n");
  for (const FeatureInfo& info : kFeatures) {
    CHECK(!MechanicActive(true, true, true, false));
    CHECK(MechanicActive(false, true, true, true));
  }
  CHECK(RowAvailable(kAllReady, Feature::kClassicSoundtrack));

  OnlineView view;
  CHECK(!view.online && !view.locked);
  CHECK(std::string(OnlineNote(view)) == kMenuOnlineNote);
  view.online = true;
  CHECK(std::string(OnlineNote(view)) == kMenuOnlineNote);
  view.locked = true;
  CHECK(std::string(OnlineNote(view)) == kMenuNextMatchNote);
  view.online = false;
  CHECK(std::string(OnlineNote(view)) == kMenuOnlineNote);
  view.online = true;

  using online::SnesStatus;
  CHECK(!OnlineReason(SnesStatus::kAvailable) && !OnlineReason(SnesStatus::kUnsupportedLocally) &&
        std::string(OnlineReason(SnesStatus::kUnsupportedByOther)) == "Unavailable: another player does not support this feature" &&
        std::string(OnlineReason(SnesStatus::kUnsupportedByService)) ==
            "Unavailable: the online service does not support this feature" &&
        std::string(OnlineReason(SnesStatus::kWaiting)) == "Waiting for players" &&
        std::string(OnlineReason(SnesStatus::kIncompatible)) == "Incompatible SNES data");

  const std::string menu = Source("src/snes_menu.cpp");
  CHECK(Has(menu, "Tr(snes::OnlineNote(online_))") && Has(menu, "online_ = SnesOnline();"));
  CHECK(Has(menu, "snes::RowStateOf(online_, row.feature, local, local && SnesFeatureEnabled(row.feature))"));
  CHECK(Has(menu, "if (snes::RowStateOf(online_, feature, true, false).usable) {\n        ToggleSnesFeatureForSession(feature);"));
  CHECK(std::string(RowStateOf(view, Feature::kClassicSoundtrack, true, true).text) == "On" &&
        std::string(RowStateOf(view, Feature::kClassicSoundtrack, true, false).text) == "Off");
  const std::string runtime = Source("src/snes_gameplay_runtime.cpp");
  CHECK(Has(runtime, "negotiation = Online().Snes();") && !Has(runtime, "ClassicSoundtrackWanted"));
  CHECK(Has(runtime, "if (feature == snes::Feature::kClassicSoundtrack && !SetClassicSoundtrackActive(enabled)) {"));
  CHECK(Has(Source("src/snes_gameplay_tmnt_r.cpp"), "SNES Gameplay: online match, using the features locked for it"));
  CHECK(!Has(Source("src/classic_soundtrack_runtime.cpp") + Source("src/classic_soundtrack/activation.cpp"), "Online("));
}

void TestModalExclusion() {
  std::printf("modal: F9 and F10 never open over each other or another overlay, input stays with the menu\n");
  CHECK(CanOpenMenu(true, false) && !CanOpenMenu(true, true) && !CanOpenMenu(false, false));

  snes::MenuModel snes_menu;
  cheats::CheatMenuModel cheat_menu;
  CHECK(snes_menu.Press(MenuKey::kF9, CanOpenMenu(true, false), kAllReady).action == Action::kOpen);
  const bool modal_open = snes_menu.open();
  CHECK(cheat_menu.Press(cheats::MenuKey::kF10, cheats::CanOpenCheatMenu(true, modal_open)).action ==
        cheats::CheatMenuModel::Action::kNone);
  CHECK(!cheat_menu.open());
  CHECK(snes_menu.Press(MenuKey::kF9, true, kAllReady).action == Action::kClose);

  CHECK(cheat_menu.Press(cheats::MenuKey::kF10, cheats::CanOpenCheatMenu(true, false)).action ==
        cheats::CheatMenuModel::Action::kOpen);
  CHECK(snes_menu.Press(MenuKey::kF9, CanOpenMenu(true, cheat_menu.open()), kAllReady).action == Action::kNone);
  CHECK(!snes_menu.open());
  CHECK(snes_menu.Press(MenuKey::kF9, CanOpenMenu(true, false), kNoRom).action == Action::kOpen);

  const std::string menu = Source("src/snes_menu.cpp");
  CHECK(Has(menu, "snes::CanOpenMenu(drawer_ != nullptr, ModalDialog::AnyOpen())"));
  CHECK(Has(menu, "class SnesMenuDialog final : public ModalDialog {"));
  CHECK(Has(menu, "constexpr size_t kInputZOrder = 1000;"));
  CHECK(Has(menu, "ImGuiWindowFlags_NoNav"));
  CHECK(Has(menu, "ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)"));
  CHECK(Has(menu, "ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)"));
  CHECK(Has(menu, "ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)"));
  CHECK(Has(menu, "const char* state = Tr(shown.text);"));
  CHECK(Has(Source("src/cheat_menu.cpp"), "cheats::CanOpenCheatMenu(drawer_ != nullptr, ModalDialog::AnyOpen())"));

  const std::string app = Source("src/tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "snes_menu_.Attach(window(), imgui_drawer());") && Has(app, "snes_menu_.Detach();"));
  CHECK(app.find("snes_menu_.Attach(") > app.find("void OnPostSetup() override {"));
  CHECK(Has(app, "return !tmnt::ModalDialog::AnyOpen() && (!imgui_drawer() || !imgui_drawer()->GetIO().WantCaptureMouse);"));
  CHECK(Has(app, "tmnt::StartSnesGameplay(user_data_root());"));

  const std::string modal = Source("src/modal_dialog.cpp");
  CHECK(Has(modal, "if (close_.Step(pressed_.any(), ImGui::GetTime())) {"));
  CHECK(Has(modal, "ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)"));
  CHECK(Has(modal, "e.virtual_key() != rex::ui::VirtualKey::kEscape"));
  CHECK(Has(Source("src/cheat_menu.cpp"), "e.virtual_key() != rex::ui::VirtualKey::kF10"));
  CHECK(Has(Source("CMakeLists.txt"), "src/snes_menu.cpp"));
}

void TestHotkeys() {
  std::printf("hotkeys: F9 stays reserved from rebinding, F10 stays the Cheats menu\n");
  const std::string keys = Source("src/keybinds.h");
  const size_t reserved = keys.find("inline bool KeyboardBindingIsReserved(ImGuiKey key) {");
  const std::string body = keys.substr(reserved, keys.find("return false;", reserved) - reserved);
  for (const char* key : {"case ImGuiKey_F2:", "case ImGuiKey_F3:", "case ImGuiKey_F7:", "case ImGuiKey_F9:",
                          "case ImGuiKey_F10:", "case ImGuiKey_F11:"}) {
    CHECK(Has(body, key));
  }
  CHECK(Has(keys, "F9 SNES menu, F10 cheats") && !Has(keys, "F9 SNES soundtrack"));
  CHECK(Has(Source("src/cheat_menu.cpp"), "VirtualKey::kF10") && !Has(Source("src/cheat_menu.cpp"), "kF9"));
  CHECK(Has(Source("src/snes_menu.cpp"), "VirtualKey::kF9") && !Has(Source("src/snes_menu.cpp"), "kF10"));
}

void TestRetiredMasterSetting() {
  std::printf("retired: tmnt_snes_gameplay has no cvar, no row, no save, no effect; an old file still loads\n");
  CHECK(kRetiredPlayerSettings.size() == 2 && kRetiredPlayerSettings[0] == "present_letterbox" &&
        kRetiredPlayerSettings[1] == "tmnt_snes_gameplay");
  const std::string old_file =
      "tmnt_snes_directional_combat = true\r\n  tmnt_snes_gameplay=false\r\ntmnt_snes_dash = false\r\n"
      "present_letterbox = true\r\ntmnt_snes_throw_control = true";
  std::string cleaned = old_file;
  for (const std::string_view key : kRetiredPlayerSettings) cleaned = RemoveSettingLines(cleaned, key);
  CHECK(cleaned == "tmnt_snes_directional_combat = true\r\ntmnt_snes_dash = false\r\ntmnt_snes_throw_control = true");
  CHECK(!Has(cleaned, "tmnt_snes_gameplay") && Has(cleaned, "tmnt_snes_dash = false"));
  CHECK(RemoveSettingLines("tmnt_snes_gameplay_data = 1\n# tmnt_snes_gameplay = false\n", "tmnt_snes_gameplay") ==
        "tmnt_snes_gameplay_data = 1\n# tmnt_snes_gameplay = false\n");
  CHECK(RemoveSettingLines("tmnt_snes_gameplay = true\n", "tmnt_snes_gameplay").empty());

  const std::string settings = Source("src/settings.cpp");
  CHECK(!Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_gameplay,") && !Has(Source("src/settings.h"), "tmnt_snes_gameplay"));
  CHECK(Has(settings, "for (const std::string_view key : kRetiredPlayerSettings) {"));
  CHECK(Has(settings, "RemoveRetiredPlayerSettings(path);"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_directional_combat, true,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_throw_control, true,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_dash, true,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_classic_soundtrack, false,"));
  const std::string keys = Between(settings, "std::vector<std::string> GameSettingKeys() {", "for (const KeyboardAction");
  for (const char* key : {"snes::kDirectionalCombatSetting,", "snes::kThrowControlSetting,", "snes::kDashSetting,",
                          "\"tmnt_classic_soundtrack\","}) {
    CHECK(Has(keys, key));
  }
  CHECK(!Has(keys, "Gameplay") && !Has(keys, "tmnt_snes_gameplay"));

  size_t files_with_key = 0;
  for (const auto& entry : fs::recursive_directory_iterator(kRoot / "src")) {
    const fs::path extension = entry.path().extension();
    if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h")) continue;
    if (entry.path().string().find("third_party") != std::string::npos) continue;
    const std::string text = ReadText(entry.path());
    std::string stripped = text;
    for (size_t at = stripped.find("tmnt_snes_gameplay_data"); at != std::string::npos; at = stripped.find("tmnt_snes_gameplay_data")) {
      stripped.erase(at, 23);
    }
    files_with_key += Has(stripped, "tmnt_snes_gameplay") ? 1 : 0;
  }
  CHECK(files_with_key == 1);
  CHECK(Has(Source("src/retired_settings.h"), "\"tmnt_snes_gameplay\""));
  CHECK(!Has(Source("config/tmnt_turtles_in_time_reshelled.toml"), "tmnt_snes"));
  CHECK(Has(Source("docs/launcher.md"), "A `tmnt_snes_gameplay` line is removed the same way."));
}

void TestLauncherPage() {
  std::printf("launcher: SNES page has the F9 help, no master row, and the four independent rows\n");
  const std::string launcher = Source("src/launcher.cpp");
  const std::string page = Between(launcher, "void PageSnes(trg::Ui& ui) {", "\n  }");
  const size_t help = page.find("ui.Help(\"Press F9 while playing to open the SNES menu. Changes made there apply only to the current session.\");");
  CHECK(help != std::string::npos && help < page.find("ui.Heading(\"SNES ROM\");"));
  CHECK(page.find("ui.Heading(\"SNES ROM\");") < page.find("ui.Heading(\"AUDIO\");") &&
        page.find("ui.Heading(\"AUDIO\");") < page.find("ui.Heading(\"GAMEPLAY\");"));

  const std::string rows = Between(launcher, "void SnesGameplayRows(trg::Ui& ui) {", "\n  }");
  const size_t combat = rows.find("ui.Toggle(\"Directional Combat\"");
  const size_t throw_control = rows.find("ui.Toggle(\"SNES Throw Control\"");
  const size_t dash = rows.find("ui.Toggle(\"SNES Dash\"");
  CHECK(combat != std::string::npos && combat < throw_control && throw_control < dash);
  CHECK(Count(rows, "ui.Toggle(") == 3 && Count(rows, "ImGui::BeginDisabled(") == 1);
  CHECK(Has(rows, "ImGui::BeginDisabled(!ready);"));
  CHECK(!Has(launcher, "SNES Gameplay\"") && !Has(launcher, "kSnesGameplaySetting") && !Has(launcher, "tmnt_snes_gameplay"));
  CHECK(Has(launcher, "ui.Toggle(\"Classic Soundtrack\", \"Plays the SNES soundtrack instead of the Re-Shelled music\","));
  CHECK(Count(launcher, "F9") == 1);
  for (const char* row : {"ui.Info(\"SNES ROM\"", "ui.Row(\"Import SNES soundtrack\"", "ui.ConfirmRow(\"Remove SNES soundtrack\""}) {
    CHECK(Has(launcher, row));
  }
}

std::set<std::string> PortugueseKeys(const std::string& table, std::vector<std::string>* problems) {
  const size_t begin = table.find("R\"PTBR(") + 7;
  const size_t end = table.find(")PTBR\"");
  std::set<std::string> keys;
  std::istringstream lines(table.substr(begin, end - begin));
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    if (!keys.insert(line.substr(0, tab)).second) problems->push_back("duplicate: " + line.substr(0, tab));
    if (tab + 1 >= line.size()) problems->push_back("empty: " + line.substr(0, tab));
  }
  return keys;
}

void TestPortuguese() {
  std::printf("pt-BR: every new static string has exactly one row, retired strings are gone\n");
  std::vector<std::string> problems;
  const std::string table = Source("src/localization/pt_br_ui_text.cpp");
  const std::set<std::string> keys = PortugueseKeys(table, &problems);
  CHECK(problems.empty());
  for (const std::string& problem : problems) std::printf("    %s\n", problem.c_str());

  const std::vector<std::string> strings = {
      "SNES", "AUDIO", "GAMEPLAY", "F9 Close", "Not saved to the launcher", "No effect in online matches",
      "Online: each player's gameplay choices apply to their own turtle", "Applies to the next online match", "Unavailable",
      "Unavailable: another player does not support this feature",
      "Unavailable: the online service does not support this feature", "Waiting for players", "Incompatible SNES data",
      "Online: every player needs compatible SNES gameplay data, and each player's choices apply to their own turtle. Classic Soundtrack is local.",
      "Cheats are unavailable while Online play is enabled.",
      "SNES ROM not imported", "SNES gameplay data unavailable: re-import the ROM in the launcher",
      "SNES gameplay data unavailable: unsupported ROM version", "SNES soundtrack: Unavailable", "SNES soundtrack: On",
      "Classic Soundtrack", "Directional Combat", "SNES Throw Control", "SNES Dash", "On", "Off",
      "Plays the SNES soundtrack instead of the Re-Shelled music",
      "Press F9 while playing to open the SNES menu. Changes made there apply only to the current session."};
  for (const std::string& english : strings) {
    CHECK(keys.count(english) == 1);
    if (!keys.count(english)) std::printf("    missing pt-BR row: %s\n", english.c_str());
  }
  CHECK(Has(table, "Pressione F9 durante o jogo para abrir o menu SNES. As altera\xC3\xA7\xC3\xB5" "es feitas ali valem apenas para a sess\xC3\xA3o atual."));

  for (const char* gone : {"SNES Gameplay", "Use gameplay mechanics inspired by the original SNES version",
                           "Directional combat", "SNES throw control", "SNES dash", "Classic soundtrack",
                           "SNES soundtrack: Off",
                           "Plays the SNES soundtrack instead of the Re-Shelled music. Press F9 during gameplay to switch between them"}) {
    CHECK(keys.count(gone) == 0);
  }

  for (const FeatureInfo& info : kFeatures) CHECK(keys.count(info.label) == 1);
  for (const char* literal : {kMenuTitle, kMenuAudioSection, kMenuGameplaySection, kMenuCloseHint, kMenuNote, kMenuOnlineNote,
                              kMenuNotImported, kMenuReimport, kMenuUnsupportedRom, kMenuSoundtrackUnavailable}) {
    CHECK(keys.count(literal) == 1);
  }
}

void TestDocs() {
  std::printf("docs: F9 is the SNES menu, never a direct soundtrack switch\n");
  const std::string input = Source("docs/input.md");
  CHECK(Has(input, "- **F9** opens and closes the SNES menu while playing;") && !Has(input, "switches between the SNES soundtrack"));
  const std::string soundtrack = Source("docs/classic-soundtrack.md");
  CHECK(Has(soundtrack, "- **F9** opens and closes the SNES menu:") && !Has(soundtrack, "F9 key ->") &&
        !Has(soundtrack, "after each F9"));
  CHECK(!Has(Source("docs/launcher.md"), "SNES Gameplay On"));
}

}  // namespace

int main() {
  TestFeatures();
  TestSessionOverrides();
  TestSessionNeverSaves();
  TestMenuModel();
  TestF9OnlyOpensAndCloses();
  TestDisabledRows();
  TestAvailabilityFromImports();
  TestGameplayEffect();
  TestSwitchedOffReset();
  TestOnline();
  TestModalExclusion();
  TestHotkeys();
  TestRetiredMasterSetting();
  TestLauncherPage();
  TestPortuguese();
  TestDocs();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
