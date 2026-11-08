// Infinite Health, Infinite Lives and Turbo Attack: the pure rules, the settings, the launcher rows and the hook wiring.
// Run: tmnt_cheats_tests

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#include "cheat_menu_rules.h"
#include "cheats.h"
#include "overlay_rules.h"
#include "quick_play_unlock.h"

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

std::string ReadFile(const char* relative) {
  std::ifstream in(std::string(TMNT_ROOT_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  std::string lf;
  for (char c : text.str()) {
    if (c != '\r') lf += c;
  }
  return lf;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

std::map<std::string, std::string> PortugueseRows() {
  const std::string file = ReadFile("src/localization/pt_br_ui_text.cpp");
  const size_t begin = file.find("R\"PTBR(");
  const size_t end = file.find(")PTBR\"");
  std::map<std::string, std::string> rows;
  if (begin == std::string::npos || end == std::string::npos) return rows;
  std::istringstream lines(file.substr(begin + 7, end - begin - 7));
  for (std::string line; std::getline(lines, line);) {
    const size_t tab = line.find('\t');
    if (tab != std::string::npos) rows[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return rows;
}

using tmnt::cheats::kTurboAttackButton;
using tmnt::cheats::kTurboPulseSeconds;

void TestOnlineSuppression() {
  std::printf("online: a session turns every cheat off, whatever is saved\n");
  using tmnt::cheats::CheatActive;
  CHECK(!CheatActive(false, false));
  CHECK(CheatActive(true, false));
  CHECK(!CheatActive(true, true));
  CHECK(!CheatActive(false, true));

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(ReadFile("src/guest_session_tmnt_r.h"), "inline constexpr uint32_t kImportIsInSession = 1145;"));
  CHECK(Has(hooks, "return Effective(cheat) && cheats::CheatActive(true, InOnlineSession(ctx, base));"));
  CHECK(Has(hooks, "CheatEnabled(ctx, base, cheats::Cheat::Health)"));
  CHECK(Has(hooks, "CheatEnabled(ctx, base, cheats::Cheat::Lives)"));
  CHECK(!Has(hooks, "Saved("));

  // An enabled session override does not beat the online gate.
  const uint32_t session = tmnt::cheats::WithOverride(0, tmnt::cheats::Cheat::Health, false, true);
  CHECK(tmnt::cheats::EffectiveCheat(session, tmnt::cheats::Cheat::Health, false));
  CHECK(!CheatActive(tmnt::cheats::EffectiveCheat(session, tmnt::cheats::Cheat::Health, false), true));
  CHECK(Has(hooks, "if (amount <= 0.0f || InOnlineSession(ctx, base)) {"));
  CHECK(Has(hooks, "driving && !InOnlineSession(ctx, base)"));
  CHECK(Has(ReadFile("src/guest_session_tmnt_r.h"), "if (!is_in_session) {\n    return true;\n  }"));
  CHECK(Has(hooks, "GuestInOnlineSession(ctx, base))"));
}

void TestHealth() {
  std::printf("health: damage passes Off, is zeroed On, a replicated death and healing pass\n");
  using tmnt::cheats::RefillAmount;
  using tmnt::cheats::ShieldedDamage;
  CHECK(ShieldedDamage(25.0f, false, false) == 25.0f);
  CHECK(ShieldedDamage(25.0f, true, false) == 0.0f);
  CHECK(ShieldedDamage(1000.0f, true, false) == 0.0f);
  CHECK(ShieldedDamage(60.0f, true, true) == 60.0f);
  CHECK(ShieldedDamage(-10.0f, true, false) == -10.0f);
  CHECK(ShieldedDamage(0.0f, true, false) == 0.0f);

  CHECK(RefillAmount(40.0f, 100.0f, true) == 60.0f);
  CHECK(RefillAmount(40.0f, 100.0f, false) == 0.0f);
  CHECK(RefillAmount(100.0f, 100.0f, true) == 0.0f);
  CHECK(RefillAmount(0.0f, 100.0f, true) == 0.0f);
  CHECK(RefillAmount(50.0f, 0.0f, true) == 0.0f);
  CHECK(RefillAmount(50.0f, std::numeric_limits<float>::infinity(), true) == 0.0f);
  CHECK(RefillAmount(std::numeric_limits<float>::quiet_NaN(), 100.0f, true) == 0.0f);

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8767C310)"));
  CHECK(Has(hooks, "ctx.f1.f64 = cheats::ShieldedDamage(damage, true, t_replicated_death > 0);"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_87673158) {\n  ++t_replicated_death;"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8767D830)"));
  CHECK(Has(hooks, "frame.ctx.f1.f64 = amount;\n  sub_8767F330(frame.ctx, base);"));
  CHECK(Has(hooks, "constexpr uint32_t kTurtleHealth = 860;"));
  CHECK(Has(hooks, "constexpr uint32_t kTurtleMaxHealth = 3260;"));
}

void TestLives() {
  std::printf("lives: a loss passes Off, is dropped On with the respawn kept, gains pass, 0 stays 0\n");
  using tmnt::cheats::LivesDelta;
  using tmnt::cheats::LivesSeenByRemoval;
  CHECK(LivesDelta(-1, false) == -1);
  CHECK(LivesDelta(-1, true) == 0);
  CHECK(LivesDelta(1, true) == 1);
  CHECK(LivesDelta(3, false) == 3);

  // BasicWorld_PlayerRemoved respawns when (read - 1) > 0 and resets the score when it reaches 0.
  for (int32_t lives : {1, 2, 5}) {
    CHECK(LivesSeenByRemoval(lives, true) - 1 == lives);
    CHECK(LivesSeenByRemoval(lives, false) - 1 == lives - 1);
  }
  CHECK(LivesSeenByRemoval(0, true) == 0);
  CHECK(LivesSeenByRemoval(-1, true) == -1);

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8757AC78)"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8758ACF8)"));
  const size_t register_lives = hooks.find("REX_HOOK_RAW(sub_8758CF00)");
  const size_t disarm = hooks.find("t_removal_armed = false;", register_lives);
  const size_t original = hooks.find("__imp__sub_8758CF00(ctx, base);", register_lives);
  CHECK(register_lives != std::string::npos && disarm < original);
  CHECK(Has(hooks, "ctx.r5.s64 = cheats::LivesDelta(ctx.r5.s32, true);"));
}

void TestTurboOffAndOtherButtons() {
  std::printf("turbo: Off, outside play and without the attack button the buttons pass unchanged\n");
  tmnt::cheats::TurboAttackState turbo;
  const uint16_t attack = kTurboAttackButton;
  for (int i = 0; i < 20; ++i) {
    const double now = i * 0.01;
    CHECK(turbo.Apply(attack, false, true, now) == attack);
    CHECK(turbo.Apply(attack | 0x1000, true, false, now) == (attack | 0x1000));
  }
  CHECK(!turbo.holding());
  for (uint16_t other : {uint16_t(0x1000), uint16_t(0x2000), uint16_t(0x8000), uint16_t(0x0010), uint16_t(0x0001)}) {
    for (int i = 0; i < 20; ++i) {
      CHECK(turbo.Apply(other, true, true, i * 0.013) == other);
    }
  }
  CHECK(kTurboAttackButton == 0x4000);
}

void TestTurboPulses() {
  std::printf("turbo: a held attack becomes press/release pulses, other held buttons stay, release stops it\n");
  tmnt::cheats::TurboAttackState turbo;
  const uint16_t held = kTurboAttackButton | 0x1000;
  CHECK(turbo.Apply(held, true, true, 0.0) == held);
  int presses = 0;
  bool was_down = true;
  for (int i = 1; i <= 100; ++i) {
    const uint16_t seen = turbo.Apply(held, true, true, i * 0.005);
    CHECK((seen & 0x1000) != 0);
    const bool down = (seen & kTurboAttackButton) != 0;
    if (down && !was_down) ++presses;
    was_down = down;
  }
  CHECK(presses >= 9 && presses <= 10);
  CHECK(turbo.pulses() >= 10);

  CHECK(turbo.Apply(0x1000, true, true, 0.51) == 0x1000);
  CHECK(!turbo.holding());
  CHECK(turbo.Apply(held, true, true, 0.52) == held);
  CHECK(turbo.Apply(held, true, true, 0.52 + kTurboPulseSeconds + 0.001) == 0x1000);
}

void TestTurboGameplayExit() {
  std::printf("turbo: leaving play mid-pulse gives the physical buttons back at once\n");
  tmnt::cheats::TurboAttackState turbo;
  turbo.Apply(kTurboAttackButton, true, true, 0.0);
  CHECK(turbo.Apply(kTurboAttackButton, true, true, 0.03) == 0);
  CHECK(turbo.Apply(kTurboAttackButton, true, false, 0.031) == kTurboAttackButton);
  CHECK(!turbo.holding());
}

void TestTurboControllersIndependent() {
  std::printf("turbo: one state per controller, so a second controller's hold does not change the first\n");
  tmnt::cheats::TurboAttackState first;
  tmnt::cheats::TurboAttackState second;
  first.Apply(kTurboAttackButton, true, true, 0.0);
  CHECK(first.Apply(kTurboAttackButton, true, true, 0.03) == 0);
  CHECK(second.Apply(kTurboAttackButton, true, true, 0.03) == kTurboAttackButton);
  CHECK(second.Apply(0, true, true, 0.031) == 0);
  CHECK(first.holding() && !second.holding());

  const std::string exe = ReadFile("src/cheats.cpp");
  CHECK(Has(exe, "std::array<TurboAttackState, kControllers> g_turbo;"));
  CHECK(Has(exe, "g_turbo[user].Apply("));
  CHECK(Has(exe, "constexpr uint32_t kJoystickUsers = 0x82BDF640;"));
  const std::string pad = ReadFile("src/achievement_menu.cpp");
  CHECK(Has(pad, "FilterGuestInput(base, user, state);\n  if (ctx.r3.u32 == 0) cheats::ApplyTurboAttack(base, user, state);"));
}

void TestGameplayGate() {
  std::printf("gate: per joystick, cleared by a paused tick, and a count that stops moving expires\n");
  using tmnt::cheats::GameplayGate;
  using tmnt::cheats::WithControllerTick;
  uint64_t gate = 0;
  gate = WithControllerTick(gate, 0, true);
  gate = WithControllerTick(gate, 2, true);
  GameplayGate seen;
  CHECK(seen.Observe(gate, 1.0) == 0b0101);
  gate = WithControllerTick(gate, 0, false);
  CHECK(seen.Observe(gate, 1.01) == 0b0100);
  CHECK(seen.Observe(gate, 1.0 + GameplayGate::kStaleSeconds + 0.01) == 0);
  gate = WithControllerTick(gate, 2, true);
  CHECK(seen.Observe(gate, 1.3) == 0b0100);
  CHECK(WithControllerTick(gate, 7, true) == gate);

  uint64_t wrap = uint64_t(0xFFFF) << 16;
  wrap = WithControllerTick(wrap, 1, true);
  CHECK(((wrap >> 16) & 0xFFFF) == 1);

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_8754BF00)"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_87572780)"));
  CHECK(Has(hooks, "const bool driving = LoadGuest32(base, vars + kControllerDisabled) == 0 && entity != 0 && !t_paused;"));
}

void TestSettings() {
  std::printf("settings: five saved cheats, Off by default, captured by the launcher and logged at start\n");
  const std::string settings = ReadFile("src/settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_cheat_infinite_health, false,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_cheat_infinite_lives, false,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_turbo_attack, false,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_turbo_strong_attack, false,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_INT32(tmnt_cheat_damage_multiplier, 0,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_cheat_super_armor, false,"));
  CHECK(Has(settings, "      cheats::kSuperArmorSetting,"));
  CHECK(std::string(tmnt::cheats::kSuperArmorSetting) == "tmnt_cheat_super_armor");
  CHECK(Has(settings, "diagnostics.launcher_gameplay.super_armor = OnOff(settings.GetBool(cheats::kSuperArmorSetting, false));"));
  CHECK(Has(settings, "diagnostics.launcher_gameplay.unlock_all_stages = OnOff(settings.GetBool(kUnlockAllStagesSetting, false));"));
  CHECK(Has(ReadFile("docs/launcher.md"), "`tmnt_cheat_super_armor`"));
  CHECK(std::string(tmnt::cheats::kTurboStrongAttackSetting) == "tmnt_turbo_strong_attack");
  CHECK(std::string(tmnt::cheats::kDamageMultiplierSetting) == "tmnt_cheat_damage_multiplier");
  CHECK(Has(ReadFile("src/settings.h"), "REXCVAR_DECLARE(int32_t, tmnt_cheat_damage_multiplier);"));
  CHECK(Has(settings, "diagnostics.launcher_gameplay.damage_multiplier = multiplier == 0 ? \"Off\" : std::to_string(multiplier) + \"x\";"));
  for (const char* key : {"cheats::kInfiniteHealthSetting,", "cheats::kInfiniteLivesSetting,", "cheats::kTurboAttackSetting,",
                          "cheats::kTurboStrongAttackSetting,", "cheats::kDamageMultiplierSetting,"}) {
    CHECK(Has(settings, std::string("      ") + key));
  }
  CHECK(Has(settings, "diagnostics.launcher_gameplay.turbo_attack = OnOff(settings.GetBool(cheats::kTurboAttackSetting, false));"));
  CHECK(!Has(settings, "tmnt_cheat_session") && !Has(settings, "kSessionSetting"));
  CHECK(std::string(tmnt::cheats::kInfiniteHealthSetting) == "tmnt_cheat_infinite_health");
  CHECK(std::string(tmnt::cheats::kInfiniteLivesSetting) == "tmnt_cheat_infinite_lives");
  CHECK(std::string(tmnt::cheats::kTurboAttackSetting) == "tmnt_turbo_attack");
  const std::string header = ReadFile("src/settings.h");
  CHECK(Has(header, "REXCVAR_DECLARE(bool, tmnt_turbo_attack);"));
  const std::string exe = ReadFile("src/cheats.cpp");
  CHECK(Has(exe, "REXCVAR_DEFINE_UINT64(tmnt_turbo_gameplay, 0,"));
  CHECK(!Has(settings, "\"tmnt_turbo_gameplay\""));
  const std::string docs = ReadFile("docs/launcher.md");
  CHECK(Has(exe, "REXCVAR_DEFINE_UINT32(tmnt_cheat_session, 0,"));
  for (const char* key : {"`tmnt_cheat_infinite_health`", "`tmnt_cheat_infinite_lives`", "`tmnt_turbo_attack`", "`tmnt_turbo_gameplay`",
                          "`tmnt_cheat_session`", "`tmnt_turbo_strong_attack`", "`tmnt_cheat_damage_multiplier`"}) {
    CHECK(Has(docs, key));
  }
}

void TestLauncherRows() {
  std::printf("launcher: Cheats is its own page after Gameplay and the Gameplay page holds none of it\n");
  const std::string launcher = ReadFile("src/launcher.cpp");
  const size_t gameplay_entry = launcher.find("{\"Gameplay\", \"Language, on-screen information, Quick Play stages and player names.\",");
  const size_t cheats_entry = launcher.find("{\"Cheats\", \"Optional gameplay modifiers\", [](trg::Ui& ui) { PageCheats(ui); }},");
  const size_t online_entry = launcher.find("{\"Online\",");
  CHECK(gameplay_entry != std::string::npos && gameplay_entry < cheats_entry && cheats_entry < online_entry);
  const size_t graphics_entry = launcher.find("{\"Graphics\",");
  CHECK(graphics_entry < gameplay_entry);

  const size_t gameplay = launcher.find("void PageGameplay(trg::Ui& ui) {");
  const size_t cheats = launcher.find("static void PageCheats(trg::Ui& ui) {");
  CHECK(gameplay != std::string::npos && gameplay < cheats);
  const std::string gameplay_page = launcher.substr(gameplay, cheats - gameplay);
  for (const char* gone : {"Infinite Health", "Infinite Lives", "Turbo Attack", "Turbo Strong Attack", "Damage Multiplier", "kInfinite",
                           "kTurbo", "kDamageMultiplier", "Section(Tr(\"Cheats\")", "Local play only"}) {
    CHECK(!Has(gameplay_page, gone));
  }
  CHECK(!Has(gameplay_page, "ClassicSoundtrackRows"));
  CHECK(Has(gameplay_page, "ui.Section(Tr(\"Local player names\"), true)"));

  const std::string rows = launcher.substr(cheats, launcher.find("\n  void ", cheats) - cheats);
  CHECK(Has(rows, "ui.Help(\"Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session.\");"));
  const size_t player = rows.find("ui.Heading(\"PLAYER\");");
  const size_t health = rows.find("ui.Toggle(\"Infinite Health\", \"Keeps player health at maximum\", cheats::kInfiniteHealthSetting, false, \"Off\", \"On\");");
  const size_t lives = rows.find("ui.Toggle(\"Infinite Lives\", \"Lives do not decrease when a player is defeated\", cheats::kInfiniteLivesSetting, false,");
  const size_t combat = rows.find("ui.Heading(\"COMBAT\");");
  const size_t turbo = rows.find("ui.Toggle(\"Turbo Attack\", \"Repeatedly attacks while the attack button is held\", cheats::kTurboAttackSetting, false,");
  const size_t damage = rows.find("ui.Choice(\"Damage Multiplier\", \"Multiplies damage dealt by players\", cheats::kDamageMultiplierSetting, \"0\",\n"
                                  "              {{\"0\", \"Off\"}, {\"2\", \"2x\"}, {\"4\", \"4x\"}}, 3);");
  const size_t strong = rows.find("ui.Toggle(\"Turbo Strong Attack\", \"Repeatedly uses the strong attack while the strong attack button is held\",\n"
                                  "              cheats::kTurboStrongAttackSetting, false, \"Off\", \"On\");");
  CHECK(player < health && health < lives && lives < combat && combat < damage && damage < turbo && turbo < strong &&
        strong != std::string::npos);
  const size_t armor = rows.find("ui.Toggle(\"Super Armor\", \"Take damage without attack interruption or knockback\", cheats::kSuperArmorSetting, false,\n"
                                 "              \"Off\", \"On\");");
  CHECK(strong < armor && armor != std::string::npos);
  const size_t spin = rows.find("ui.Toggle(\"Spin Attack\", spin_help.c_str(), cheats::kSpinAttackSetting, false, \"Off\", \"On\");");
  CHECK(armor < spin && spin != std::string::npos);
  CHECK(Has(rows, "Tr(\"Use Spin Attack at any time with B or\")"));
  CHECK(Has(rows, "KeyboardBindingSlashLabel(SavedKeyboardBinding(ui.settings(), kSpinAttackKeyAction))"));
  CHECK(!Has(rows, "with B\"") && !Has(rows, "Use the Spin Attack"));
  CHECK(!Has(rows, "Unlock all stages") && !Has(rows, "kUnlockAllStages"));
  size_t controls = 0;
  for (const char* control : {"ui.Toggle(", "ui.Choice("}) {
    for (size_t at = rows.find(control); at != std::string::npos; at = rows.find(control, at + 1)) ++controls;
  }
  CHECK(controls == 7);
}

void TestSpinAttackHelp() {
  std::printf("spin attack help: controller B and the mapped keyboard key\n");
  CHECK(std::string(tmnt::cheats::kSpinAttackControllerHelp) == "Controller: B");
  CHECK(std::string(tmnt::cheats::kSpinAttackKeyboardHelp) == "Keyboard:");
  const std::string menu = ReadFile("src/cheat_menu.cpp");
  const size_t help = menu.find("void DrawSpinAttackHelp()");
  CHECK(help != std::string::npos);
  const std::string body = menu.substr(help, menu.find("\n  void DrawRow", help) - help);
  CHECK(Has(body, "cheats::Cheat::SpinAttack"));
  CHECK(Has(body, "UsableKeyboardBinding(rex::cvar::GetFlagByName(kSpinAttackKeyAction.key), kSpinAttackKeyAction)"));
  CHECK(Has(body, "kSpinAttackControllerHelp") && Has(body, "kSpinAttackKeyboardHelp"));
  CHECK(Has(menu, "      DrawSpinAttackHelp();"));
}

void TestPortuguese() {
  std::printf("pt-BR: every new launcher string has its row\n");
  const std::map<std::string, std::string> rows = PortugueseRows();
  const std::map<std::string, std::string> expected = {
      {"Cheats", "Trapa\xC3\xA7" "as"},
      {"Infinite Health", "Vida infinita"},
      {"Infinite Lives", "Vidas infinitas"},
      {"Turbo Attack", "Ataque turbo"},
      {"Keeps player health at maximum", "Mant\xC3\xA9m a vida do jogador no m\xC3\xA1ximo"},
      {"Lives do not decrease when a player is defeated", "As vidas n\xC3\xA3o diminuem quando o jogador \xC3\xA9 derrotado"},
      {"Repeatedly attacks while the attack button is held",
       "Repete o ataque enquanto o bot\xC3\xA3o de ataque estiver pressionado"},
      {"Optional gameplay modifiers", "Modificadores opcionais de jogabilidade"},
      {"Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session.",
       "Pressione F10 durante o jogo para abrir o menu de Trapa\xC3\xA7" "as. As altera\xC3\xA7\xC3\xB5" "es feitas ali valem apenas para a sess\xC3\xA3o atual."},
      {"PLAYER", "JOGADOR"},
      {"COMBAT", "COMBATE"},
      {"CHEATS", "TRAPA\xC3\x87" "AS"},
      {"F10 Close", "F10 Fechar"},
      {"Not saved to the launcher", "N\xC3\xA3o salvo no launcher"},
      {"No effect in online matches", "Sem efeito em partidas online"},
      {"Turbo Strong Attack", "Ataque forte turbo"},
      {"Repeatedly uses the strong attack while the strong attack button is held",
       "Repete o ataque forte enquanto o bot\xC3\xA3o estiver pressionado"},
      {"Damage Multiplier", "Multiplicador de dano"},
      {"Multiplies damage dealt by players", "Multiplica o dano causado pelos jogadores"},
      {"Super Armor", "Super Armor"},
      {"Take damage without attack interruption or knockback", "Recebe dano sem interromper ataques ou sofrer knockback"},
      {"Spin Attack", "Ataque girat\xC3\xB3rio"},
      {"Use Spin Attack at any time with B or", "Use o Ataque girat\xC3\xB3rio com B ou"},
      {"Controller: B", "Controle: B"},
      {"Keyboard:", "Teclado:"},
      {"Unlock all stages", "Desbloquear todas as fases"},
      {"Makes every Quick Play stage selectable without changing saved progress",
       "Torna todas as fases do Quick Play selecion\xC3\xA1veis sem alterar o progresso salvo"},
  };
  for (const auto& [english, portuguese] : expected) {
    const auto row = rows.find(english);
    CHECK(row != rows.end() && row->second == portuguese);
    if (row == rows.end()) std::printf("    missing pt-BR row: %s\n", english.c_str());
  }
  CHECK(rows.find("Local play only: cheats have no effect in online matches.") == rows.end());

  const std::string table = ReadFile("src/localization/pt_br_ui_text.cpp");
  for (const auto& [english, portuguese] : expected) {
    size_t count = 0;
    for (size_t at = table.find("\n" + english + "\t"); at != std::string::npos; at = table.find("\n" + english + "\t", at + 1)) ++count;
    CHECK(count == 1);
    if (count != 1) std::printf("    pt-BR row appears %zu times: %s\n", count, english.c_str());
  }

  const std::string launcher = ReadFile("src/launcher.cpp");
  CHECK(Has(launcher, "{\"Gameplay\", \"Language, on-screen information, Quick Play stages and player names.\","));
}

void TestSessionOverrides() {
  std::printf("session: an override beats the saved value, none follows it, nothing is saved\n");
  using namespace tmnt::cheats;
  for (const CheatInfo& info : kCheats) {
    CHECK(EffectiveCheat(0u, info.cheat, false) == false);
    CHECK(EffectiveCheat(0u, info.cheat, true) == true);
    CHECK(OverrideOf(0u, info.cheat) == Override::kFollowsSaved);

    uint32_t session = ToggledOverride(0u, info.cheat, false);
    CHECK(EffectiveCheat(session, info.cheat, false));
    CHECK(OverrideOf(session, info.cheat) == Override::kOn);
    session = ToggledOverride(session, info.cheat, false);
    CHECK(session == 0u);

    session = ToggledOverride(0u, info.cheat, true);
    CHECK(!EffectiveCheat(session, info.cheat, true));
    CHECK(OverrideOf(session, info.cheat) == Override::kOff);
  }

  uint32_t session = ToggledOverride(0u, Cheat::Health, false);
  CHECK(EffectiveCheat(session, Cheat::Health, false));
  CHECK(!EffectiveCheat(session, Cheat::Lives, false));
  CHECK(!EffectiveCheat(session, Cheat::Turbo, false));
  session = ToggledOverride(session, Cheat::Turbo, false);
  CHECK(EffectiveCheat(session, Cheat::Health, false) && EffectiveCheat(session, Cheat::Turbo, false));
  CHECK(!EffectiveCheat(session, Cheat::Lives, false));
  CHECK(OverrideOf(session, Cheat::Lives) == Override::kFollowsSaved);

  CHECK(WithOverride(0u, Cheat::Lives, false, false) == 0u);
  CHECK(kCheats.size() == 6 && kCheatCount == 6);
  CHECK(std::string(kCheats[0].label) == "Infinite Health" && std::string(kCheats[1].label) == "Infinite Lives" &&
        std::string(kCheats[2].label) == "Turbo Attack" && std::string(kCheats[3].label) == "Turbo Strong Attack" &&
        std::string(kCheats[4].label) == "Super Armor" && std::string(kCheats[5].label) == "Spin Attack");

  const std::string menu = ReadFile("src/cheat_menu.cpp") + ReadFile("src/cheats.cpp") + ReadFile("src/cheat_menu.h");
  for (const char* write : {"Save(", "SaveNow(", "settings_", "TextFileSettings", "settings.toml", "kInfiniteHealthSetting",
                            "kTurboAttackSetting"}) {
    CHECK(!Has(menu, write));
  }
  CHECK(Has(ReadFile("src/cheats.cpp"), "rex::cvar::SetFlagByName(kSessionSetting, std::to_string(next));"));
  CHECK(Has(ReadFile("src/cheats_tmnt_r.cpp"), "rex::cvar::Query<uint32_t>(cheats::kSessionSetting)"));
  CHECK(Has(ReadFile("src/cheats.cpp"), "EffectiveCheat(REXCVAR_GET(tmnt_cheat_session), cheat, SavedValue(cheat))"));
}

void TestSessionEffects() {
  std::printf("session: the three cheats read the same effective value, and Health refills on the turn On\n");
  using namespace tmnt::cheats;
  const uint32_t on = ToggledOverride(0u, Cheat::Health, false);
  CHECK(RefillAmount(30.0f, 100.0f, EffectiveCheat(0u, Cheat::Health, false)) == 0.0f);
  CHECK(RefillAmount(30.0f, 100.0f, EffectiveCheat(on, Cheat::Health, false)) == 70.0f);
  CHECK(RefillAmount(30.0f, 100.0f, EffectiveCheat(0u, Cheat::Health, true)) == 70.0f);
  CHECK(ShieldedDamage(10.0f, EffectiveCheat(on, Cheat::Health, false), false) == 0.0f);
  CHECK(ShieldedDamage(10.0f, EffectiveCheat(ToggledOverride(on, Cheat::Health, false), Cheat::Health, false), false) == 10.0f);

  const uint32_t lives_on = ToggledOverride(0u, Cheat::Lives, false);
  CHECK(LivesDelta(-1, EffectiveCheat(lives_on, Cheat::Lives, false)) == 0);
  CHECK(LivesDelta(-1, EffectiveCheat(0u, Cheat::Lives, false)) == -1);

  const uint32_t turbo_on = ToggledOverride(0u, Cheat::Turbo, false);
  TurboAttackState turbo;
  turbo.Apply(kTurboAttackButton, EffectiveCheat(turbo_on, Cheat::Turbo, false), true, 0.0);
  CHECK(turbo.Apply(kTurboAttackButton, EffectiveCheat(turbo_on, Cheat::Turbo, false), true, 0.03) == 0);
  CHECK(turbo.Apply(kTurboAttackButton, EffectiveCheat(0u, Cheat::Turbo, false), true, 0.031) == kTurboAttackButton);

  // A new process has no overrides: the cvar default is 0 and is not a saved key.
  CHECK(EffectiveCheat(0u, Cheat::Health, false) == false);

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "if (!Effective(cheats::Cheat::Health) || !ValidGuest(turtle)) {"));
  CHECK(Has(hooks, "const bool turbo = Effective(cheats::Cheat::Turbo) || Effective(cheats::Cheat::TurboStrong);"));
  CHECK(Has(ReadFile("src/cheats.cpp"), "g_turbo[user].Apply(buttons, Enabled(Cheat::Turbo), gameplay, now);"));
}

void TestMenuModel() {
  std::printf("F10: open, close, Escape, B, navigation, change, exactly five rows\n");
  using namespace tmnt::cheats;
  using Action = CheatMenuModel::Action;
  CheatMenuModel menu;
  CHECK(!menu.open());
  CHECK(menu.Press(MenuKey::kEnter, true).action == Action::kNone);
  CHECK(menu.Press(MenuKey::kPadA, true).action == Action::kNone);
  CHECK(menu.Press(MenuKey::kEscape, true).action == Action::kNone);
  CHECK(menu.Press(MenuKey::kDown, true).action == Action::kNone && menu.selected() == 0);

  CHECK(menu.Press(MenuKey::kF10, false).action == Action::kNone && !menu.open());
  CHECK(menu.Press(MenuKey::kF10, true).action == Action::kOpen && menu.open());
  CHECK(menu.Press(MenuKey::kF10, true).action == Action::kClose && !menu.open());

  for (MenuKey close : {MenuKey::kEscape, MenuKey::kPadB}) {
    menu.Press(MenuKey::kF10, true);
    CHECK(menu.Press(close, true).action == Action::kClose && !menu.open());
  }

  menu.Press(MenuKey::kF10, true);
  CHECK(menu.selected() == 0);
  for (size_t expected = 1; expected <= kCheatMenuRowCount; ++expected) {
    menu.Press(MenuKey::kDown, true);
    CHECK(menu.selected() == expected % kCheatMenuRowCount);
  }
  menu.Press(MenuKey::kUp, true);
  CHECK(menu.selected() == 6);
  menu.Press(MenuKey::kUp, true);
  CHECK(menu.selected() == 5);
  menu.Select(1);

  for (MenuKey change : {MenuKey::kEnter, MenuKey::kPadA}) {
    const auto result = menu.Press(change, true);
    CHECK(result.action == Action::kChange && result.row == 1);
    CHECK(menu.open());
  }
  menu.Select(4);
  CHECK(menu.Press(MenuKey::kPadA, true).row == 4);
  menu.Select(9);
  CHECK(menu.selected() == 4);

  menu.Press(MenuKey::kEscape, true);
  menu.Press(MenuKey::kF10, true);
  CHECK(menu.selected() == 0);
  menu.Closed();
  CHECK(!menu.open());

  CHECK(kCheatMenuRowCount == 7);
  const char* labels[] = {"Infinite Health", "Infinite Lives", "Damage Multiplier", "Turbo Attack", "Turbo Strong Attack",
                          "Super Armor", "Spin Attack"};
  for (size_t i = 0; i < kCheatMenuRowCount; ++i) {
    CHECK(std::string(kCheatMenuRows[i].label) == labels[i]);
    CHECK(kCheatMenuRows[i].multiplier == (i == 2));
  }
  CHECK(kCheatMenuRows[0].cheat == Cheat::Health && kCheatMenuRows[1].cheat == Cheat::Lives &&
        kCheatMenuRows[3].cheat == Cheat::Turbo && kCheatMenuRows[4].cheat == Cheat::TurboStrong &&
        kCheatMenuRows[5].cheat == Cheat::SuperArmor && kCheatMenuRows[6].cheat == Cheat::SpinAttack);
  for (const CheatMenuRow& row : kCheatMenuRows) CHECK(std::string(row.label) != "Unlock all stages");
  CHECK(kCheatMenuPlayerRow == 0 && kCheatMenuCombatRow == 2);
  CHECK(std::string(DamageMultiplierLabel(0)) == "Off" && std::string(DamageMultiplierLabel(2)) == "2x" &&
        std::string(DamageMultiplierLabel(4)) == "4x");

  const std::string source = ReadFile("src/cheat_menu.cpp");
  CHECK(Has(source, "for (size_t i = 0; i < cheats::kCheatMenuRowCount; ++i) {"));
  CHECK(Has(source, "DrawRow(cheats::kCheatMenuRows[i], i,"));
  CHECK(Has(source, "cheats::CycleDamageMultiplierForSession();"));
  CHECK(Has(source, "cheats::ToggleForSession(row.cheat);"));
  // F10 never changes a cheat: it only opens and closes.
  const size_t key = source.find("void CheatMenu::OnKeyDown(");
  CHECK(key != std::string::npos && !Has(source.substr(key), "ToggleForSession") && !Has(source.substr(key), "CycleDamage"));
  CHECK(Has(source, "ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)"));
  CHECK(Has(source, "ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)"));
  CHECK(Has(source, "ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)"));
  CHECK(Has(source, "e.virtual_key() != rex::ui::VirtualKey::kF10"));
  CHECK(Has(source, "Tr(on ? \"On\" : \"Off\")"));
}

void TestMenuOverlays() {
  std::printf("F10: not over the launcher or another overlay, and only after the game runs\n");
  using tmnt::cheats::CanOpenCheatMenu;
  CHECK(CanOpenCheatMenu(true, false));
  CHECK(!CanOpenCheatMenu(true, true));
  CHECK(!CanOpenCheatMenu(false, false));

  tmnt::cheats::CheatMenuModel menu;
  CHECK(menu.Press(tmnt::cheats::MenuKey::kF10, CanOpenCheatMenu(true, true)).action ==
        tmnt::cheats::CheatMenuModel::Action::kNone);

  const std::string source = ReadFile("src/cheat_menu.cpp");
  CHECK(Has(source, "cheats::CanOpenCheatMenu(drawer_ != nullptr, ModalDialog::AnyOpen())"));
  CHECK(Has(source, "class CheatMenuDialog final : public ModalDialog {"));
  const std::string app = ReadFile("src/tmnt_turtles_in_time_reshelled_app.h");
  const size_t launcher = app.find("tmnt::ShowLauncher(");
  const size_t attach = app.find("cheat_menu_.Attach(window(), imgui_drawer());");
  const size_t post_setup = app.find("void OnPostSetup() override {");
  CHECK(launcher != std::string::npos && post_setup != std::string::npos && post_setup > launcher && attach > post_setup);
  CHECK(Has(app, "cheat_menu_.Detach();"));
  CHECK(Has(ReadFile("src/modal_dialog.cpp"), "g_open.store(this, std::memory_order_release);"));
  CHECK(Has(ReadFile("src/achievements_overlay.cpp"), ": ModalDialog(drawer),"));
  CHECK(Has(app, "if (achievements_ || tmnt::ModalDialog::AnyOpen() ||"));
}

void TestInputGating() {
  std::printf("F10: the game reads no input while the menu is open, and the closing press stays out\n");
  const std::string app = ReadFile("src/tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "return !tmnt::ModalDialog::AnyOpen() && (!imgui_drawer() || !imgui_drawer()->GetIO().WantCaptureMouse);"));
  const std::string modal = ReadFile("src/modal_dialog.cpp");
  CHECK(Has(modal, "if (close_.Step(pressed_.any(), ImGui::GetTime())) {"));
  CHECK(Has(modal, "ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)"));
  CHECK(Has(modal, "e.virtual_key() != rex::ui::VirtualKey::kEscape"));

  tmnt::CloseGate gate;
  CHECK(!gate.Step(true, 0.0));
  gate.Request();
  CHECK(!gate.Step(true, 1.0));
  CHECK(!gate.Step(true, 1.5));
  CHECK(gate.Step(false, 1.6));

  tmnt::CloseGate stuck;
  stuck.Request();
  CHECK(!stuck.Step(true, 5.0));
  CHECK(stuck.Step(true, 5.0 + tmnt::kReleaseTimeoutSeconds));

  const std::string menu = ReadFile("src/cheat_menu.cpp");
  CHECK(Has(menu, "e.set_handled(true);"));
  CHECK(Has(menu, "constexpr size_t kInputZOrder = 1000;"));
  CHECK(Has(menu, "ImGuiWindowFlags_NoNav"));

  const std::string keys = ReadFile("src/keybinds.h");
  const size_t reserved = keys.find("inline bool KeyboardBindingIsReserved(ImGuiKey key) {");
  const size_t end = keys.find("return false;", reserved);
  CHECK(reserved != std::string::npos && Has(keys.substr(reserved, end - reserved), "case ImGuiKey_F10:"));
}

void TestTurboStrong() {
  std::printf("turbo strong: Y pulses on its own, independent of X and of other controllers\n");
  using namespace tmnt::cheats;
  constexpr uint16_t kX = kTurboAttackButton;
  constexpr uint16_t kY = kTurboStrongAttackButton;
  CHECK(kY == 0x8000 && kX == 0x4000);

  TurboAttackState strong(kY);
  for (int i = 0; i < 10; ++i) CHECK(strong.Apply(kY, false, true, i * 0.01) == kY);
  CHECK(strong.Apply(kY, true, false, 0.2) == kY);
  CHECK(!strong.holding());

  int presses = 0;
  bool was_down = true;
  CHECK(strong.Apply(kY, true, true, 1.0) == kY);
  for (int i = 1; i <= 100; ++i) {
    const bool down = (strong.Apply(kY, true, true, 1.0 + i * 0.005) & kY) != 0;
    if (down && !was_down) ++presses;
    was_down = down;
  }
  CHECK(presses >= 9 && presses <= 10);
  CHECK(strong.Apply(0, true, true, 1.51) == 0 && !strong.holding());
  CHECK(strong.Apply(kX, true, true, 1.52) == kX);

  // The pad hook runs the X state, then the Y state, on the same buttons.
  TurboAttackState x(kX);
  TurboAttackState y(kY);
  const auto both = [&](uint16_t buttons, bool x_on, bool y_on, double now) {
    return y.Apply(x.Apply(buttons, x_on, true, now), y_on, true, now);
  };
  CHECK(both(kX, true, true, 0.0) == kX);
  CHECK(both(kX, true, true, 0.03) == 0);
  CHECK(!y.holding());
  CHECK(both(kX | kY, true, true, 0.031) == kY);
  CHECK(y.holding() && x.holding());
  // Each stream keeps its own phase: X is pressed again at 0.05, Y released at 0.056.
  CHECK(both(kX | kY, true, true, 0.06) == kX);
  CHECK(both(kY, true, true, 0.07) == 0 && !x.holding() && y.holding());
  CHECK(both(kY, true, false, 0.3) == kY && !y.holding());

  std::array<TurboAttackState, kControllers> pads{TurboAttackState(kY), TurboAttackState(kY), TurboAttackState(kY),
                                                   TurboAttackState(kY)};
  pads[0].Apply(kY, true, true, 0.0);
  CHECK(pads[0].Apply(kY, true, true, 0.03) == 0);
  CHECK(pads[1].Apply(kY, true, true, 0.03) == kY);
  CHECK(pads[2].Apply(0, true, true, 0.03) == 0 && !pads[2].holding());

  const uint32_t session = ToggledOverride(0u, Cheat::TurboStrong, false);
  CHECK(EffectiveCheat(session, Cheat::TurboStrong, false) && !EffectiveCheat(session, Cheat::Turbo, false));
  CHECK(!EffectiveCheat(0u, Cheat::TurboStrong, false));
  CHECK(!CheatActive(EffectiveCheat(session, Cheat::TurboStrong, false), true));

  const std::string exe = ReadFile("src/cheats.cpp");
  CHECK(Has(exe, "std::array<TurboAttackState, kControllers> g_turbo_strong{"));
  CHECK(Has(exe, "applied = g_turbo_strong[user].Apply(applied, Enabled(Cheat::TurboStrong), gameplay, now);"));
}

void TestDamageMultiplierSession() {
  std::printf("damage multiplier: saved value, F10 cycling Off, 2x, 4x, and the saved value is never written\n");
  using namespace tmnt::cheats;
  CHECK(ParseDamageMultiplier(0) == 0 && ParseDamageMultiplier(2) == 2 && ParseDamageMultiplier(4) == 4);
  CHECK(ParseDamageMultiplier(3) == 0 && ParseDamageMultiplier(-2) == 0 && ParseDamageMultiplier(8) == 0);
  CHECK(EffectiveMultiplier(0u, 0) == 0 && EffectiveMultiplier(0u, 2) == 2 && EffectiveMultiplier(0u, 4) == 4);

  uint32_t session = 0;
  session = CycledMultiplier(session, 0);
  CHECK(EffectiveMultiplier(session, 0) == 2);
  session = CycledMultiplier(session, 0);
  CHECK(EffectiveMultiplier(session, 0) == 4);
  session = CycledMultiplier(session, 0);
  CHECK(EffectiveMultiplier(session, 0) == 0 && session == 0u);

  session = CycledMultiplier(0u, 4);
  CHECK(EffectiveMultiplier(session, 4) == 0);
  session = CycledMultiplier(session, 4);
  CHECK(EffectiveMultiplier(session, 4) == 2);
  session = CycledMultiplier(session, 4);
  CHECK(EffectiveMultiplier(session, 4) == 4 && session == 0u);

  // The multiplier override and the toggle overrides do not touch each other.
  uint32_t mixed = ToggledOverride(0u, Cheat::TurboStrong, false);
  mixed = CycledMultiplier(mixed, 0);
  CHECK(EffectiveCheat(mixed, Cheat::TurboStrong, false) && EffectiveMultiplier(mixed, 0) == 2);
  for (const CheatInfo& info : kCheats) {
    if (info.cheat != Cheat::TurboStrong) CHECK(!EffectiveCheat(mixed, info.cheat, false));
  }
  CHECK(EffectiveMultiplier(ToggledOverride(0u, Cheat::Health, false), 0) == 0);

  const std::string exe = ReadFile("src/cheats.cpp");
  CHECK(Has(exe, "const uint32_t next = CycledMultiplier(REXCVAR_GET(tmnt_cheat_session), SavedMultiplier());"));
  CHECK(!Has(exe, "SetFlagByName(kDamageMultiplierSetting") && !Has(exe, "SetFlagByName(\"tmnt_cheat_damage_multiplier\""));
}

void TestDamageMath() {
  std::printf("damage multiplier: only positive player hits and slams are scaled, safely\n");
  using namespace tmnt::cheats;
  CHECK(ScaledDamage(10.0f, 0) == 10.0f);
  CHECK(ScaledDamage(10.0f, 1) == 10.0f);
  CHECK(ScaledDamage(10.0f, 2) == 20.0f);
  CHECK(ScaledDamage(10.0f, 4) == 40.0f);
  CHECK(ScaledDamage(0.0f, 4) == 0.0f);
  CHECK(ScaledDamage(-5.0f, 4) == -5.0f);
  CHECK(ScaledDamage(-100000.0f, 4) == -100000.0f);
  CHECK(std::isnan(ScaledDamage(std::numeric_limits<float>::quiet_NaN(), 4)));
  CHECK(std::isinf(ScaledDamage(std::numeric_limits<float>::infinity(), 4)));
  CHECK(ScaledDamage(std::numeric_limits<float>::max(), 4) == std::numeric_limits<float>::max());

  CHECK(ScalesDamage(kEntityHit, true));
  CHECK(ScalesDamage(kEntityGetSlammed, true));
  CHECK(!ScalesDamage(kEntityHit, false));
  CHECK(!ScalesDamage(kEntityGetSlammed, false));
  CHECK(!ScalesDamage(37, true));  // ENTITY_THROWTOSCREEN carries no damage
  CHECK(!ScalesDamage(10, true));
  CHECK(kEntityHit == 3 && kEntityGetSlammed == 22);

  // A boss takes the same Entity_Notify hit as any enemy, so its normal damage follows the multiplier.
  CHECK(ScaledDamage(25.0f, EffectiveMultiplier(0u, 2)) == 50.0f);
  CHECK(!CheatActive(EffectiveMultiplier(0u, 4) > 1, true));
}

void TestDamageHooks() {
  std::printf("damage multiplier: every enemy and boss Entity_Notify, Turtle source only, never the turtle's own\n");
  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  for (const char* receiver : {"sub_87633B98)  // FootSoldier", "sub_8765E8D0)  // CharacterBehaviorBase", "sub_87652618)  // RockSoldier",
                               "sub_8763EC68)  // PizzaMonster", "sub_87641380)  // BoxingRobot", "sub_87667CF8)  // BoxingRobotBlue",
                               "sub_87645050)  // Annihilator", "sub_87660E90)  // MiniCopter", "sub_87612018)  // Velociraptor",
                               "sub_87621398)  // KrangFlyingSaucer", "sub_876578D0)  // Baxter", "sub_87664520)  // LeatherHead",
                               "sub_87690610)  // MetalHead", "sub_8769D710)  // CementMan", "sub_87649680)  // Tokka",
                               "sub_876A38E8)  // Rahzar", "sub_876A8068)  // SuperKrangBoss", "sub_87696890)  // Shredder"}) {
    CHECK(Has(hooks, std::string("TMNT_DAMAGE_RECEIVER(") + receiver));
  }
  size_t receivers = 0;
  for (size_t at = hooks.find("\nTMNT_DAMAGE_RECEIVER("); at != std::string::npos; at = hooks.find("\nTMNT_DAMAGE_RECEIVER(", at + 1)) ++receivers;
  CHECK(receivers == 18);
  CHECK(!Has(hooks, "TMNT_DAMAGE_RECEIVER(sub_87677CF8)"));
  CHECK(Has(hooks, "constexpr uint32_t kTurtleEntityNotify = 0x87677CF8;"));
  CHECK(Has(hooks, "return ValidGuest(vtable) && LoadGuest32(base, vtable + 8) == kTurtleEntityNotify ? script : 0;"));
  CHECK(Has(hooks, "constexpr uint32_t kStockDamage = 20;") && Has(hooks, "constexpr uint32_t kStockSource = 40;"));
  CHECK(Has(hooks, "if (!cheats::ScalesDamage(note, IsTurtleObject(ctx, base, source)) || InOnlineSession(ctx, base)) {"));
  CHECK(Has(hooks, "if (t_damage_receiver > 0 || !ValidGuest(stock)) {"));
  CHECK(Has(hooks, "if (scaled == damage || source == 0) {"));
  const size_t scale = hooks.find("ScalePlayerDamage(ctx, base);   \\");
  const size_t original = hooks.find("__imp__##name(ctx, base);", scale);
  CHECK(scale != std::string::npos && scale < original);
}

void TestSuperArmor() {
  std::printf("super armor: the hit still deals damage, the state stays, lethal hits, grabs and deaths keep the game's path\n");
  using namespace tmnt::cheats;
  constexpr uint32_t kAttack = 2;  // ENTITY_ATTACK; idle, walk, run and jump behave the same
  CHECK(!SuperArmorAbsorbs(false, 60.0f, kAttack, kHitStrong));
  CHECK(SuperArmorAbsorbs(true, 60.0f, kAttack, kHitStrong));
  CHECK(SuperArmorAbsorbs(true, 0.5f, 0, kHitLight));
  CHECK(!SuperArmorAbsorbs(true, 0.0f, kAttack, kHitStrong));  // a lethal hit dies normally
  CHECK(!SuperArmorAbsorbs(true, -3.0f, kAttack, kHitStrong));
  CHECK(!SuperArmorAbsorbs(true, std::numeric_limits<float>::quiet_NaN(), kAttack, kHitStrong));
  CHECK(!SuperArmorAbsorbs(true, 60.0f, kEntityDying, kHitStrong));
  CHECK(!SuperArmorAbsorbs(true, 60.0f, kEntityGrabbed, kHitStrong));
  CHECK(!SuperArmorAbsorbs(true, 60.0f, kEntityGrabbedHit, kHitStrong));
  CHECK(kEntityDying == 4 && kEntityGrabbed == 32 && kEntityGrabbedHit == 36);

  // Every routed combat and hazard hit, Saulo's grab, wrecking ball and mine findings included; the Spinning Pizza
  // pick-up and the turtle's own slam detector are not hit reactions.
  for (uint32_t type = 0; type <= 19; ++type) {
    const bool covered = type != kHitFullSpinPowerUp && type != kHitSlamAcquire;
    CHECK(SuperArmorCovers(type) == covered);
    CHECK(SuperArmorAbsorbs(true, 40.0f, kAttack, type) == covered);
  }
  CHECK(kHitFlattened == 3 && kHitSewerTrap == 13 && kHitBubbleKrang == 16 && kHitFullSpinPowerUp == 6);

  // Turtle_RequestHitRouting after Turtle_TakeDamage: "no hit react" and not ultimate ends the hit; otherwise a hit,
  // concuss, special-hit or death track follows. Super Armor answers both questions for an absorbed hit.
  enum class Route { kAbsorbed, kReactionTrack, kDeathTrack };
  const auto route = [](bool stock_no_react, bool ultimate, bool absorbs, float health) {
    const bool no_react = stock_no_react || absorbs;
    const bool lets_through = ultimate && !absorbs;
    if (no_react && !lets_through) return Route::kAbsorbed;
    return health <= 0.0f ? Route::kDeathTrack : Route::kReactionTrack;
  };
  const auto absorbs = [](bool on, float health, uint32_t state, uint32_t type) {
    return SuperArmorAbsorbs(on, health, state, type);
  };
  // Off: stock behaviour.
  CHECK(route(false, false, absorbs(false, 50.0f, kAttack, kHitLight), 50.0f) == Route::kReactionTrack);
  CHECK(route(false, true, absorbs(false, 50.0f, kAttack, kHitStrong), 50.0f) == Route::kReactionTrack);
  // On: normal, knockdown/special and the previous bug, a nonlethal ultimate combat hit.
  CHECK(route(false, false, absorbs(true, 50.0f, kAttack, kHitLight), 50.0f) == Route::kAbsorbed);
  CHECK(route(false, true, absorbs(true, 50.0f, kAttack, kHitStrong), 50.0f) == Route::kAbsorbed);
  CHECK(route(true, true, absorbs(true, 50.0f, kAttack, kHitStrong), 50.0f) == Route::kAbsorbed);  // jump attack, stagger
  CHECK(route(false, true, absorbs(true, 50.0f, 0, kHitSewerTrap), 50.0f) == Route::kAbsorbed);  // surf mine
  CHECK(route(false, true, absorbs(true, 50.0f, kAttack, kHitFlattened), 50.0f) == Route::kAbsorbed);  // wrecking ball
  CHECK(route(false, false, absorbs(true, 50.0f, kAttack, kHitBubbleKrang), 50.0f) == Route::kAbsorbed);
  CHECK(route(false, true, absorbs(true, 0.0f, kAttack, kHitFlattened), 0.0f) == Route::kDeathTrack);
  CHECK(route(false, true, absorbs(false, 50.0f, kAttack, kHitFlattened), 50.0f) == Route::kReactionTrack);
  // Lethal hits, grabs and scripted hits keep the game's path.
  CHECK(route(false, true, absorbs(true, 0.0f, kAttack, kHitStrong), 0.0f) == Route::kDeathTrack);
  CHECK(route(false, false, absorbs(true, 0.0f, kAttack, kHitLight), 0.0f) == Route::kDeathTrack);
  CHECK(route(false, true, absorbs(true, 50.0f, kEntityGrabbed, kHitStrong), 50.0f) == Route::kReactionTrack);

  // Damage is not part of the rule: it was applied before the question, and Super Armor is not Infinite Health.
  CHECK(ShieldedDamage(30.0f, EffectiveCheat(ToggledOverride(0u, Cheat::SuperArmor, false), Cheat::Health, false), false) == 30.0f);
  const uint32_t armor = ToggledOverride(0u, Cheat::SuperArmor, false);
  CHECK(EffectiveCheat(armor, Cheat::SuperArmor, false) && !EffectiveCheat(armor, Cheat::Health, false));
  CHECK(!EffectiveCheat(0u, Cheat::SuperArmor, false) && EffectiveCheat(0u, Cheat::SuperArmor, true));
  CHECK(!CheatActive(EffectiveCheat(armor, Cheat::SuperArmor, false), true));
  CHECK(EffectiveMultiplier(armor, 0) == 0 && EffectiveMultiplier(CycledMultiplier(armor, 0), 0) == 2);
  CHECK(EffectiveCheat(CycledMultiplier(armor, 0), Cheat::SuperArmor, false));

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_876746E0) {"));
  CHECK(Has(hooks, "constexpr uint32_t kTurtleState = 3256;"));
  CHECK(Has(hooks, "if (t_hit_type < 0 || !ValidGuest(turtle) || !Effective(cheats::Cheat::SuperArmor)) {"));
  const size_t armor_hook = hooks.find("REX_HOOK_RAW(sub_876746E0) {");
  const size_t original = hooks.find("__imp__sub_876746E0(ctx, base);", armor_hook);
  const size_t online = hooks.find("InOnlineSession(ctx, base)", armor_hook);
  const size_t armored = hooks.find("t_armored = true;", armor_hook);
  const size_t answer = hooks.find("ctx.r3.u64 = 1;", armor_hook);
  CHECK(armor_hook < original && original < online && online < armored && armored < answer);

  const size_t routing = hooks.find("REX_HOOK_RAW(sub_87676438) {");
  CHECK(routing != std::string::npos && routing < armor_hook);
  CHECK(Has(hooks, "  t_hit_type = ctx.r4.u32;\n  t_armored = false;\n  __imp__sub_87676438(ctx, base);\n  t_hit_type = outer_type;\n  t_armored = outer_armored;"));
  const size_t ultimate = hooks.find("REX_HOOK_RAW(sub_87676378) {");
  CHECK(ultimate != std::string::npos &&
        Has(hooks.substr(ultimate, 160), "__imp__sub_87676378(ctx, base);\n  if (t_armored) {\n    ctx.r3.u64 = 0;"));
  // Damage, death, movement and velocity code are left alone.
  size_t armor_reads = 0;
  for (size_t at = hooks.find("Cheat::SuperArmor"); at != std::string::npos; at = hooks.find("Cheat::SuperArmor", at + 1)) ++armor_reads;
  CHECK(armor_reads == 2);
  const std::string damage = hooks.substr(hooks.find("REX_HOOK_RAW(sub_8767C310)"), 400);
  CHECK(!Has(damage, "SuperArmor"));
}

void TestSuperArmorGrab() {
  std::printf("super armor: an enemy grab is refused the game's way, the turtle's own grabs and deaths are not touched\n");
  using namespace tmnt::cheats;
  CHECK(kGrabRefused == -1);
  CHECK(SuperArmorRefusesGrab(true, 30.0f, kEntityAttack));
  CHECK(SuperArmorRefusesGrab(true, 30.0f, kEntityIdle));
  CHECK(!SuperArmorRefusesGrab(false, 30.0f, kEntityAttack));  // Off: the grab goes on
  CHECK(!SuperArmorRefusesGrab(true, 0.0f, kEntityIdle));
  CHECK(!SuperArmorRefusesGrab(true, 30.0f, kEntityDying));
  CHECK(!SuperArmorRefusesGrab(true, 30.0f, kEntityGrabbed));  // already held: the game releases it

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  const size_t grab = hooks.find("REX_HOOK_RAW(sub_87681010) {");
  const size_t refuse = hooks.find("ctx.r3.s64 = cheats::kGrabRefused;\n    return;", grab);
  const size_t original = hooks.find("__imp__sub_87681010(ctx, base);", grab);
  CHECK(grab != std::string::npos && grab < refuse && refuse < original);
  CHECK(Has(hooks, "cheats::SuperArmorRefusesGrab(true, LoadGuestFloat(base, vars + kTurtleHealth),"));
  CHECK(Has(hooks.substr(hooks.find("static bool RefusesGrab("), 500), "!InOnlineSession(ctx, base)"));
  // The turtle's own slam and throw (Turtle_NotifySlamTarget) are not hooked.
  CHECK(!Has(hooks, "REX_HOOK_RAW(sub_8767FA88)"));
}

void TestSpinAttack() {
  std::printf("spin attack: one press of B starts one Spinning Pizza spin during play, through the B mapping\n");
  using namespace tmnt::cheats;
  CHECK(std::string(kSpinAttackSetting) == "tmnt_cheat_spin_attack");
  CHECK(kSpinAttackIoButton == 2);
  for (uint32_t state : {kEntityIdle, kEntityAttack, kEntityWalk, kEntityRun, kEntityAttackSlide, kEntityAttackRecovery}) {
    CHECK(SpinAttackAllowed(true, true, 50.0f, state));
    CHECK(!SpinAttackAllowed(false, true, 50.0f, state));  // Off: B does nothing new
    CHECK(!SpinAttackAllowed(true, false, 50.0f, state));  // only a fresh press
  }
  for (uint32_t state : {kEntityDying, kEntityGrabbed, kEntityGrabbedHit, kEntityAttackFullSpin, 5u, 10u, 3u, 16u, 41u, 43u, 48u}) {
    CHECK(!SpinAttackAllowed(true, true, 50.0f, state));
  }
  CHECK(!SpinAttackAllowed(true, true, 0.0f, kEntityIdle));

  // A held B is pressed once: the engine's JustPressed is true for one controller tick only.
  int spins = 0;
  bool was_down = false;
  for (bool down : {true, true, true, true, false, false, true, true, false}) {
    const bool just = down && !was_down;
    was_down = down;
    if (SpinAttackAllowed(true, just, 50.0f, kEntityWalk)) ++spins;
  }
  CHECK(spins == 2);

  const uint32_t session = ToggledOverride(0u, Cheat::SpinAttack, false);
  CHECK(EffectiveCheat(session, Cheat::SpinAttack, false) && !EffectiveCheat(0u, Cheat::SpinAttack, false));
  CHECK(!CheatActive(EffectiveCheat(session, Cheat::SpinAttack, false), true));

  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "constexpr uint32_t kImportButtonJustPressed = 501;"));
  CHECK(Has(hooks, "if (!ButtonJustPressed(ctx, base, joystick, cheats::kSpinAttackIoButton)) {"));
  CHECK(Has(hooks, "  sub_87674A40(frame.ctx, base);"));
  CHECK(!Has(hooks, "sub_87674B80"));  // the pick-up path, which counts a power-up in the profile
  CHECK(Has(hooks, "const bool playing = driving && !InOnlineSession(ctx, base);"));
  CHECK(Has(hooks, "  if (spin && playing) {\n    RequestSpinAttack(ctx, base, joystick, entity);"));

  // B reaches the guest through ReXGlue's pad and keybind_b; nothing in the port listens for a key of its own.
  const std::string keys = ReadFile("src/keybinds.h");
  CHECK(Has(keys, "{\"keybind_b\", \"B\", \"Quote,Backspace\"},"));
  for (const char* file : {"src/cheats.cpp", "src/window_host.cpp", "src/tmnt_turtles_in_time_reshelled_app.h"}) {
    const std::string text = ReadFile(file);
    CHECK(!Has(text, "VirtualKey::kOem7") && !Has(text, "ImGuiKey_Apostrophe") && !Has(text, "SpinAttack"));
  }
  CHECK(!Has(ReadFile("src/cheat_menu.cpp"), "VirtualKey::kOem7") && !Has(ReadFile("src/cheat_menu.cpp"), "ImGuiKey_Apostrophe"));
  const std::string settings = ReadFile("src/settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_cheat_spin_attack, false,"));
  CHECK(Has(settings, "      cheats::kSpinAttackSetting,"));
  CHECK(Has(settings, "diagnostics.launcher_gameplay.spin_attack = OnOff(settings.GetBool(cheats::kSpinAttackSetting, false));"));
  CHECK(Has(ReadFile("docs/launcher.md"), "`tmnt_cheat_spin_attack`"));
}

void TestQuickPlay() {
  std::printf("quick play: Off keeps progress, On lists every stage, Off again relocks first, nothing stored\n");
  using tmnt::QuickPlayUnlock;
  using tmnt::kStageSelectInitEvent;
  CHECK(kStageSelectInitEvent == 166 && tmnt::kQuickPlayStageCount == 13 && tmnt::kStageSelectTrackOffset == 4);
  CHECK(std::string(tmnt::kUnlockAllStagesSetting) == "tmnt_unlock_all_stages");

  QuickPlayUnlock unlock;
  auto plan = unlock.Open(kStageSelectInitEvent, false, false);
  CHECK(!plan.lock_all_first && !plan.unlock_all_after);  // Off from the start: the game's own messages only
  plan = unlock.Open(kStageSelectInitEvent + 1, true, false);
  CHECK(!plan.lock_all_first && !plan.unlock_all_after);  // other UI events never
  plan = unlock.Open(kStageSelectInitEvent, true, false);
  CHECK(!plan.lock_all_first && plan.unlock_all_after && unlock.showing_all());
  plan = unlock.Open(kStageSelectInitEvent, true, false);  // reopened, or a profile reloaded: all again
  CHECK(!plan.lock_all_first && plan.unlock_all_after);
  plan = unlock.Open(kStageSelectInitEvent, false, false);  // Off again: relock, then the game's real unlocks
  CHECK(plan.lock_all_first && !plan.unlock_all_after && !unlock.showing_all());
  plan = unlock.Open(kStageSelectInitEvent, false, false);
  CHECK(!plan.lock_all_first && !plan.unlock_all_after);
  plan = unlock.Open(kStageSelectInitEvent, true, true);  // online: never unlocked
  CHECK(!plan.unlock_all_after && !plan.lock_all_first);
  unlock.Open(kStageSelectInitEvent, true, false);
  plan = unlock.Open(kStageSelectInitEvent, true, true);  // online after an offline unlock: the real list comes back
  CHECK(plan.lock_all_first && !plan.unlock_all_after);

  const std::string hooks = ReadFile("src/live_hooks_tmnt_r.cpp");
  const size_t hook = hooks.find("REX_HOOK_RAW(sub_875BA998) {");
  const size_t lock = hooks.find("if (quick_play.lock_all_first) QueueEveryStage(ctx, base, __imp__sub_875B10B0, self, stage_track);", hook);
  const size_t original = hooks.find("__imp__sub_875BA998(ctx, base);", hook);
  const size_t unlock_all = hooks.find("if (quick_play.unlock_all_after) QueueEveryStage(ctx, base, __imp__sub_875B0ED0, self, stage_track);", hook);
  CHECK(hook != std::string::npos && hook < lock && lock < original && original < unlock_all && unlock_all != std::string::npos);
  CHECK(Has(hooks, "const uint32_t stage_track = LoadBE32(base, var + kStageSelectTrackOffset);"));
  CHECK(Has(hooks, "REXLOG_INFO(\"Unlock all stages: {}\", plan.unlock_all_after ? \"On\" : \"Off\");"));
  const size_t begin = hooks.find("void QueueEveryStage(");
  const std::string body = hooks.substr(begin, hooks.find("\n}  // namespace", begin) - begin);
  for (const char* banned : {"Profile", "Achievement", "Save", "Store", "SetFlag", "STORE"}) {
    CHECK(!Has(body, banned));
  }
  for (const char* gone : {"DEBUG_SAULO", "soundtrack testing", "kQuickPlayUnlockBuild", "UnlocksAllQuickPlayStages"}) {
    CHECK(!Has(hooks, gone));
    CHECK(!Has(ReadFile("src/quick_play_unlock.h"), gone));
  }
  const std::string cmake = ReadFile("CMakeLists.txt");
  CHECK(!Has(cmake, "QUICKPLAY") && !Has(cmake, "TMNT_DEBUG_QUICKPLAY_UNLOCK"));
  CHECK(!Has(ReadFile("docs/classic-soundtrack.md"), "soundtrack testing"));

  const std::string settings = ReadFile("src/settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_unlock_all_stages, false,"));
  CHECK(Has(settings, "      kUnlockAllStagesSetting,"));
  const std::string launcher = ReadFile("src/launcher.cpp");
  const size_t gameplay = launcher.find("void PageGameplay(trg::Ui& ui) {");
  const size_t row = launcher.find("ui.Toggle(\"Unlock all stages\", \"Makes every Quick Play stage selectable without changing saved progress\",\n"
                                   "              kUnlockAllStagesSetting, false, \"Off\", \"On\");", gameplay);
  const size_t cheats = launcher.find("static void PageCheats(trg::Ui& ui) {");
  CHECK(gameplay < row && row < cheats);
  CHECK(Has(ReadFile("docs/launcher.md"), "`tmnt_unlock_all_stages`"));
}

void TestBuild() {
  std::printf("build: the hooks are in TMNT_r, Turbo is in the game executable, this suite is a target\n");
  const std::string cmake = ReadFile("CMakeLists.txt");
  const size_t dll = cmake.find("target_sources(tmnt_turtles_in_time_reshelled_TMNT_r PRIVATE");
  CHECK(dll != std::string::npos && cmake.find("src/cheats_tmnt_r.cpp", dll) != std::string::npos);
  const size_t exe = cmake.find("set(TMNT_SOURCES");
  CHECK(exe != std::string::npos && cmake.find("    src/cheats.cpp\n", exe) < cmake.find(")", exe));
  CHECK(Has(cmake, "add_executable(tmnt_cheats_tests EXCLUDE_FROM_ALL tests/cheats_tests.cpp)"));
  CHECK(exe != std::string::npos && cmake.find("    src/cheat_menu.cpp\n", exe) < cmake.find(")", exe));
  for (const char* file : {"src/cheats_tmnt_r.cpp", "src/cheats.cpp", "src/cheat_menu.cpp", "src/cheat_menu_rules.h",
                           "src/quick_play_unlock.h", "src/live_hooks_tmnt_r.cpp", "src/guest_session_tmnt_r.h"}) {
    CHECK(!Has(ReadFile(file), "DEBUG_SAULO"));
  }
}

}  // namespace

void TestOnlinePlayEnabledBlocksCheats() {
  std::printf("online play enabled: F10 and every cheat are off from the moment it is on, match or not\n");
  using namespace tmnt::cheats;
  CHECK(CheatsAllowed(false) && !CheatsAllowed(true));
  CHECK(!CheatsAllowed(false, true) && !CheatsAllowed(true, true));

  // Online play on, whatever state the service and the party are in: the menu never opens.
  for (const bool restart : {false, true}) {
    CheatMenuModel menu;
    const bool online_enabled = !restart;
    const bool blocked = !CheatsAllowed(online_enabled);
    const CheatMenuModel::Result result = menu.Press(MenuKey::kF10, true, blocked);
    CHECK(online_enabled ? result.action == CheatMenuModel::Action::kUnavailable && !menu.open()
                         : result.action == CheatMenuModel::Action::kOpen && menu.open());
  }

  // A held key arrives as one fresh press and then repeats; only the fresh press reaches the model.
  const std::string menu_source = ReadFile("src/cheat_menu.cpp");
  CHECK(Has(menu_source, "  if (e.prev_state()) {\n    return;\n  }\n  const bool can_open"));
  CHECK(Has(menu_source, "bool CheatsBlocked() { return !cheats::CheatsAllowed(rex::cvar::Query<bool>(kOnlineEnabledSetting)); }"));
  CHECK(!Has(menu_source, "Online()") && !Has(menu_source, "ONL_") && !Has(menu_source, "OnlineRunning"));
  CHECK(Has(menu_source, "REXLOG_INFO(\"Cheats menu blocked: Online play enabled\");"));

  // An open menu closes when Online play turns on, and its keys change nothing meanwhile.
  CheatMenuModel open;
  CHECK(open.Press(MenuKey::kF10, true, false).action == CheatMenuModel::Action::kOpen);
  CHECK(open.Press(MenuKey::kEnter, true, true).action == CheatMenuModel::Action::kNone);
  CHECK(Has(menu_source, "if (CheatsBlocked()) {\n      RequestClose();\n      return;\n    }"));

  // Every cheat hook goes through the one gate, which ignores the match state when Online play is enabled.
  const std::string hooks = ReadFile("src/cheats_tmnt_r.cpp");
  CHECK(Has(hooks, "const bool enabled = rex::cvar::Query<bool>(kOnlineEnabledSetting);"));
  CHECK(Has(hooks, "!cheats::CheatsAllowed(enabled, enabled ? false : GuestInOnlineSession(ctx, base))"));
  CHECK(Has(hooks, "return Effective(cheat) && cheats::CheatActive(true, InOnlineSession(ctx, base));"));
  for (const char* use : {"InOnlineSession(ctx, base)) {", "|| InOnlineSession(ctx, base)", "!InOnlineSession(ctx, base)"}) {
    CHECK(Has(hooks, use));
  }
  CHECK(!Has(hooks, "GuestInOnlineSession(ctx, base);\n  if (!online)"));
  CHECK(hooks.find("GuestInOnlineSession(") == hooks.rfind("GuestInOnlineSession("));
  for (const Cheat cheat : {Cheat::Health, Cheat::Lives, Cheat::Turbo, Cheat::TurboStrong, Cheat::SuperArmor, Cheat::SpinAttack}) {
    CHECK(!CheatActive(EffectiveCheat(WithOverride(0, cheat, false, true), cheat, true), !CheatsAllowed(true)));
  }
  CHECK(!(EffectiveMultiplier(WithMultiplier(0, 0, 4), 0) > 1 && CheatsAllowed(true)));

  // F9 and the SNES menu do not follow this gate.
  const std::string snes_menu = ReadFile("src/snes_menu.cpp");
  CHECK(!Has(snes_menu, "CheatsAllowed") && !Has(snes_menu, "kOnlineEnabledSetting"));
}

int main() {
  TestOnlinePlayEnabledBlocksCheats();
  TestOnlineSuppression();
  TestHealth();
  TestLives();
  TestTurboOffAndOtherButtons();
  TestTurboPulses();
  TestTurboGameplayExit();
  TestTurboControllersIndependent();
  TestGameplayGate();
  TestSettings();
  TestLauncherRows();
  TestSpinAttackHelp();
  TestPortuguese();
  TestSessionOverrides();
  TestSessionEffects();
  TestMenuModel();
  TestMenuOverlays();
  TestInputGating();
  TestTurboStrong();
  TestDamageMultiplierSession();
  TestDamageMath();
  TestDamageHooks();
  TestSuperArmor();
  TestSuperArmorGrab();
  TestSpinAttack();
  TestQuickPlay();
  TestBuild();
  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
