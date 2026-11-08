#include "cheats.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "settings.h"

REXCVAR_DEFINE_UINT64(tmnt_turbo_gameplay, 0, "TMNT",
                      "Turtle controllers in unpaused play, written by the game for Turbo Attack; not a setting");
REXCVAR_DEFINE_UINT32(tmnt_cheat_session, 0, "TMNT",
                      "Cheat overrides of the in-game F10 menu for this session; not a setting, never saved");

namespace tmnt::cheats {
namespace {

// default.xex's joystick-to-user table: sub_824ECA80 reads joystick N's buttons from XInput user table[N].
constexpr uint32_t kJoystickUsers = 0x82BDF640;
// X_INPUT_STATE: a 32-bit packet number, then the gamepad's 16-bit buttons.
constexpr uint32_t kButtonsOffset = 4;

std::mutex g_mutex;
GameplayGate g_gate;
std::array<TurboAttackState, kControllers> g_turbo;
std::array<TurboAttackState, kControllers> g_turbo_strong{
    TurboAttackState(kTurboStrongAttackButton), TurboAttackState(kTurboStrongAttackButton),
    TurboAttackState(kTurboStrongAttackButton), TurboAttackState(kTurboStrongAttackButton)};

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool UserDrivesTurtle(const uint8_t* base, uint32_t user, uint32_t joysticks) {
  for (uint32_t joystick = 0; joystick < kControllers; ++joystick) {
    if ((joysticks & (1u << joystick)) != 0 && LoadBE32(base, kJoystickUsers + joystick * 4) == user) {
      return true;
    }
  }
  return false;
}

bool SavedValue(Cheat cheat) { return rex::cvar::Query<bool>(kCheats[static_cast<size_t>(cheat)].setting); }

int SavedMultiplier() { return ParseDamageMultiplier(REXCVAR_GET(tmnt_cheat_damage_multiplier)); }

}  // namespace

bool Enabled(Cheat cheat) { return EffectiveCheat(REXCVAR_GET(tmnt_cheat_session), cheat, SavedValue(cheat)); }

void ToggleForSession(Cheat cheat) {
  const uint32_t next = ToggledOverride(REXCVAR_GET(tmnt_cheat_session), cheat, SavedValue(cheat));
  rex::cvar::SetFlagByName(kSessionSetting, std::to_string(next));
  REXLOG_INFO("Cheats: {} {} for this session", kCheats[static_cast<size_t>(cheat)].label,
              Enabled(cheat) ? "On" : "Off");
}

int DamageMultiplier() { return EffectiveMultiplier(REXCVAR_GET(tmnt_cheat_session), SavedMultiplier()); }

void CycleDamageMultiplierForSession() {
  const uint32_t next = CycledMultiplier(REXCVAR_GET(tmnt_cheat_session), SavedMultiplier());
  rex::cvar::SetFlagByName(kSessionSetting, std::to_string(next));
  REXLOG_INFO("Cheats: Damage Multiplier {}x for this session", DamageMultiplier());
}

void ApplyTurboAttack(uint8_t* base, uint32_t user, uint32_t state) {
  if (!state || user >= kControllers) {
    return;
  }
  const double now = NowSeconds();
  const uint16_t buttons = LoadBE16(base, state + kButtonsOffset);
  std::lock_guard lock(g_mutex);
  const uint32_t joysticks = g_gate.Observe(REXCVAR_GET(tmnt_turbo_gameplay), now);
  const bool gameplay = UserDrivesTurtle(base, user, joysticks);
  uint16_t applied = g_turbo[user].Apply(buttons, Enabled(Cheat::Turbo), gameplay, now);
  applied = g_turbo_strong[user].Apply(applied, Enabled(Cheat::TurboStrong), gameplay, now);
  if (applied != buttons) {
    StoreBE16(base, state + kButtonsOffset, applied);
  }
}

}  // namespace tmnt::cheats
