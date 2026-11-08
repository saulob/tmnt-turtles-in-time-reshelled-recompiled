#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "cheats.h"
#include "guest_memory.h"
#include "guest_session_tmnt_r.h"
#include "online_setting.h"

using namespace tmnt;

REX_EXTERN(sub_87674A40);

namespace {

// Turtle script variables at *(turtle + 64), as Turtle_TakeDamage reads them.
constexpr uint32_t kTurtleVars = 64;
constexpr uint32_t kTurtleHealth = 860;
constexpr uint32_t kTurtleMaxHealth = 3260;
constexpr uint32_t kTurtleState = 3256;

// TJC_Main's controller variables at *(track + 36).
constexpr uint32_t kControllerVars = 36;
constexpr uint32_t kControllerJoystick = 0;
constexpr uint32_t kControllerEntity = 4;
constexpr uint32_t kControllerDisabled = 16;

// MDF_Script(object) as the scripts compile it: the object's script modifier, then the script instance.
constexpr uint32_t kImportObjectModifier = 1855;
constexpr uint32_t kImportModifierScript = 11;
constexpr uint32_t kImportButtonJustPressed = 501;  // IO_JoystickButtonJustPressed(joystick, button)

// Entity_Notify(this, stock&) receives the stock in r4: note type +0, f32_1 +20, po_Obj1 +40.
constexpr uint32_t kStockNote = 0;
constexpr uint32_t kStockDamage = 20;
constexpr uint32_t kStockSource = 40;
// Every Turtle class has this Entity_Notify in vtable slot +8.
constexpr uint32_t kTurtleEntityNotify = 0x87677CF8;

thread_local int t_replicated_death = 0;
thread_local int t_damage_receiver = 0;
thread_local int64_t t_hit_type = -1;
thread_local bool t_armored = false;
thread_local bool t_removal_armed = false;
thread_local bool t_in_controller = false;
thread_local bool t_paused = true;

std::atomic<uint64_t> g_gameplay{0};
std::atomic<bool> g_online_logged{false};

bool ValidGuest(uint32_t address) { return address >= 0x10000 && address < 0xFFFFF000; }

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  return LoadBE32(GuestHost(const_cast<uint8_t*>(base), address), 0);
}

float LoadGuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadGuest32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreBE32(GuestHost(base, address), 0, bits);
}

bool Effective(cheats::Cheat cheat) {
  return cheats::EffectiveCheat(rex::cvar::Query<uint32_t>(cheats::kSessionSetting), cheat,
                                rex::cvar::Query<bool>(cheats::kCheats[static_cast<size_t>(cheat)].setting));
}

int Multiplier() {
  return cheats::EffectiveMultiplier(rex::cvar::Query<uint32_t>(cheats::kSessionSetting),
                                     rex::cvar::Query<int32_t>(cheats::kDamageMultiplierSetting));
}

// Side calls, so the hooked function's own arguments stay as the game passed them.
uint32_t TurtleScriptOf(PPCContext& parent, uint8_t* base, uint32_t object) {
  rex::CallFrame frame(parent);
  frame.ctx.lr = parent.lr;
  PPCFunc* object_modifier = ResolveGuestImport(frame, base, kImportObjectModifier);
  if (!object_modifier) {
    return 0;
  }
  frame.ctx.r3.u64 = object;
  frame.ctx.r4.u64 = 0;
  frame.ctx.r5.u64 = 0;
  object_modifier(frame.ctx, base);
  const uint32_t modifier = frame.ctx.r3.u32;
  if (!ValidGuest(modifier)) {
    return 0;
  }
  PPCFunc* modifier_script = ResolveGuestImport(frame, base, kImportModifierScript);
  if (!modifier_script) {
    return 0;
  }
  frame.ctx.r3.u64 = modifier;
  modifier_script(frame.ctx, base);
  const uint32_t script = frame.ctx.r3.u32;
  if (!ValidGuest(script)) {
    return 0;
  }
  const uint32_t vtable = LoadGuest32(base, script);
  return ValidGuest(vtable) && LoadGuest32(base, vtable + 8) == kTurtleEntityNotify ? script : 0;
}

bool IsTurtleObject(PPCContext& parent, uint8_t* base, uint32_t object) {
  return TurtleScriptOf(parent, base, object) != 0;
}

bool ButtonJustPressed(PPCContext& parent, uint8_t* base, uint32_t joystick, uint32_t button) {
  rex::CallFrame frame(parent);
  frame.ctx.lr = parent.lr;
  PPCFunc* just_pressed = ResolveGuestImport(frame, base, kImportButtonJustPressed);
  if (!just_pressed) {
    return false;
  }
  frame.ctx.r3.u64 = joystick;
  frame.ctx.r4.u64 = button;
  just_pressed(frame.ctx, base);
  return frame.ctx.r3.u32 != 0;
}

// True whenever Online play is enabled, or the guest is in an online session, so nothing applies before a match starts.
bool InOnlineSession(PPCContext& ctx, uint8_t* base) {
  const bool enabled = rex::cvar::Query<bool>(kOnlineEnabledSetting);
  const bool online = !cheats::CheatsAllowed(enabled, enabled ? false : GuestInOnlineSession(ctx, base));
  if (!online) {
    g_online_logged.store(false, std::memory_order_relaxed);
  } else if (!g_online_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("Cheats: not applied while Online play is enabled or in an online session");
  }
  return online;
}

bool CheatEnabled(PPCContext& ctx, uint8_t* base, cheats::Cheat cheat) {
  return Effective(cheat) && cheats::CheatActive(true, InOnlineSession(ctx, base));
}

// The outermost Entity_Notify of a hit only: a notify it makes in turn is the receiver's own event.
void ScalePlayerDamage(PPCContext& ctx, uint8_t* base) {
  const uint32_t stock = ctx.r4.u32;
  if (t_damage_receiver > 0 || !ValidGuest(stock)) {
    return;
  }
  const uint32_t note = LoadGuest32(base, stock + kStockNote);
  if (note != cheats::kEntityHit && note != cheats::kEntityGetSlammed) {
    return;
  }
  const int multiplier = Multiplier();
  const float damage = LoadGuestFloat(base, stock + kStockDamage);
  const float scaled = cheats::ScaledDamage(damage, multiplier);
  const uint32_t source = LoadGuest32(base, stock + kStockSource);
  if (scaled == damage || source == 0) {
    return;
  }
  if (!cheats::ScalesDamage(note, IsTurtleObject(ctx, base, source)) || InOnlineSession(ctx, base)) {
    return;
  }
  StoreGuestFloat(base, stock + kStockDamage, scaled);
}

// Turtle_RequestFullSpin_S(this): the Spinning Pizza's spin, without the power-up count the pick-up adds to the profile.
void RequestSpinAttack(PPCContext& ctx, uint8_t* base, uint32_t joystick, uint32_t entity) {
  if (!ButtonJustPressed(ctx, base, joystick, cheats::kSpinAttackIoButton)) {
    return;
  }
  const uint32_t turtle = TurtleScriptOf(ctx, base, entity);
  const uint32_t vars = turtle ? LoadGuest32(base, turtle + kTurtleVars) : 0;
  if (!ValidGuest(vars) || !cheats::SpinAttackAllowed(true, true, LoadGuestFloat(base, vars + kTurtleHealth),
                                                      LoadGuest32(base, vars + kTurtleState))) {
    return;
  }
  rex::CallFrame frame(ctx);
  frame.ctx.lr = ctx.lr;
  frame.ctx.r3.u64 = turtle;
  sub_87674A40(frame.ctx, base);
}

void PublishController(uint32_t joystick, bool driving) {
  const uint64_t current = g_gameplay.load(std::memory_order_relaxed);
  const uint64_t next = cheats::WithControllerTick(current, joystick, driving);
  if (next == current) {
    return;
  }
  g_gameplay.store(next, std::memory_order_relaxed);
  rex::cvar::SetFlagByName(cheats::kTurboGameplaySetting, std::to_string(next));
}

}  // namespace

// Turtle_TakeDamage(this, health, hit counter): the only place a turtle's health goes down. Only Turtle objects run it.
REX_EXTERN(__imp__sub_8767C310);
REX_HOOK_RAW(sub_8767C310) {
  const float damage = static_cast<float>(ctx.f1.f64);
  if (damage > 0.0f && CheatEnabled(ctx, base, cheats::Cheat::Health)) {
    ctx.f1.f64 = cheats::ShieldedDamage(damage, true, t_replicated_death > 0);
  }
  __imp__sub_8767C310(ctx, base);
}

// Turtle_RequestDying_S(this, stock): a replicated death, which empties the health through Turtle_TakeDamage.
REX_EXTERN(__imp__sub_87673158);
REX_HOOK_RAW(sub_87673158) {
  ++t_replicated_death;
  __imp__sub_87673158(ctx, base);
  --t_replicated_death;
}

REX_EXTERN(sub_8767F330);

// Turtle_PostNox(this): every turtle, once per frame. A damaged turtle is refilled through Turtle_DoCheatHeal(this,
// amount), the game's own clamped heal that also updates the HUD.
REX_EXTERN(__imp__sub_8767D830);
REX_HOOK_RAW(sub_8767D830) {
  const uint32_t turtle = ctx.r3.u32;
  __imp__sub_8767D830(ctx, base);
  if (!Effective(cheats::Cheat::Health) || !ValidGuest(turtle)) {
    return;
  }
  const uint32_t vars = LoadGuest32(base, turtle + kTurtleVars);
  if (!ValidGuest(vars)) {
    return;
  }
  const float amount = cheats::RefillAmount(LoadGuestFloat(base, vars + kTurtleHealth),
                                            LoadGuestFloat(base, vars + kTurtleMaxHealth), true);
  if (amount <= 0.0f || InOnlineSession(ctx, base)) {
    return;
  }
  rex::CallFrame frame(ctx);
  frame.ctx.lr = ctx.lr;
  frame.ctx.r3.u64 = turtle;
  frame.ctx.f1.f64 = amount;
  sub_8767F330(frame.ctx, base);
}

// Turtle_RequestHitRouting(this, type, hit counter, health, &source): every hit a turtle reacts to starts here, and
// after Turtle_TakeDamage it asks Turtle_IsNoHitReact and then Turtle_IsUltimateHit before any hit state.
REX_EXTERN(__imp__sub_87676438);
REX_HOOK_RAW(sub_87676438) {
  const int64_t outer_type = t_hit_type;
  const bool outer_armored = t_armored;
  t_hit_type = ctx.r4.u32;
  t_armored = false;
  __imp__sub_87676438(ctx, base);
  t_hit_type = outer_type;
  t_armored = outer_armored;
}

// Turtle_IsNoHitReact(this): only Turtle_RequestHitRouting asks it.
REX_EXTERN(__imp__sub_876746E0);
REX_HOOK_RAW(sub_876746E0) {
  const uint32_t turtle = ctx.r3.u32;
  __imp__sub_876746E0(ctx, base);
  if (t_hit_type < 0 || !ValidGuest(turtle) || !Effective(cheats::Cheat::SuperArmor)) {
    return;
  }
  const uint32_t vars = LoadGuest32(base, turtle + kTurtleVars);
  if (!ValidGuest(vars) ||
      !cheats::SuperArmorAbsorbs(true, LoadGuestFloat(base, vars + kTurtleHealth), LoadGuest32(base, vars + kTurtleState),
                                 uint32_t(t_hit_type)) ||
      InOnlineSession(ctx, base)) {
    return;
  }
  t_armored = true;
  ctx.r3.u64 = 1;
}

// Turtle_IsUltimateHit(this, type): only Turtle_RequestHitRouting asks it, to let a hit through "no hit react".
REX_EXTERN(__imp__sub_87676378);
REX_HOOK_RAW(sub_87676378) {
  __imp__sub_87676378(ctx, base);
  if (t_armored) {
    ctx.r3.u64 = 0;
  }
}

static bool RefusesGrab(PPCContext& ctx, uint8_t* base, uint32_t turtle) {
  if (!ValidGuest(turtle) || !Effective(cheats::Cheat::SuperArmor)) {
    return false;
  }
  const uint32_t vars = LoadGuest32(base, turtle + kTurtleVars);
  return ValidGuest(vars) &&
         cheats::SuperArmorRefusesGrab(true, LoadGuestFloat(base, vars + kTurtleHealth),
                                       LoadGuest32(base, vars + kTurtleState)) &&
         !InOnlineSession(ctx, base);
}

// Turtle_RequestGrabbed(this, &enemy): an enemy's grab asks the turtle to accept it; the snap bone means yes.
REX_EXTERN(__imp__sub_87681010);
REX_HOOK_RAW(sub_87681010) {
  if (RefusesGrab(ctx, base, ctx.r3.u32)) {
    ctx.r3.s64 = cheats::kGrabRefused;
    return;
  }
  __imp__sub_87681010(ctx, base);
}

// BasicWorld_PlayerRemoved(this, player): the only caller of PlayerProfile_RegisterLives, with -1 for a local player.
REX_EXTERN(__imp__sub_8757AC78);
REX_HOOK_RAW(sub_8757AC78) {
  const bool outer = t_removal_armed;
  t_removal_armed = CheatEnabled(ctx, base, cheats::Cheat::Lives);
  __imp__sub_8757AC78(ctx, base);
  t_removal_armed = outer;
}

// PlayerProfile_GetActiveSlotLives(this, slot).
REX_EXTERN(__imp__sub_8758ACF8);
REX_HOOK_RAW(sub_8758ACF8) {
  __imp__sub_8758ACF8(ctx, base);
  if (t_removal_armed) {
    ctx.r3.u64 = static_cast<uint32_t>(cheats::LivesSeenByRemoval(ctx.r3.s32, true));
  }
}

// PlayerProfile_RegisterLives(this, player, delta). Disarmed first: the lives-changed checks it runs read the real count.
REX_EXTERN(__imp__sub_8758CF00);
REX_HOOK_RAW(sub_8758CF00) {
  if (t_removal_armed) {
    t_removal_armed = false;
    ctx.r5.s64 = cheats::LivesDelta(ctx.r5.s32, true);
  }
  __imp__sub_8758CF00(ctx, base);
}

// Pause_IsPaused(this): TJC_Main's first call, before anything else in its tick.
REX_EXTERN(__imp__sub_87572780);
REX_HOOK_RAW(sub_87572780) {
  __imp__sub_87572780(ctx, base);
  if (t_in_controller) {
    t_in_controller = false;
    t_paused = ctx.r3.u32 != 0;
  }
}

// TJC_Main(this): a turtle controller's tick. It reads moves and attacks only while it is enabled, drives a turtle and
// the game is not paused; that is the play Turbo Attack may act in.
REX_EXTERN(__imp__sub_8754BF00);
REX_HOOK_RAW(sub_8754BF00) {
  const uint32_t track = ctx.r3.u32;
  t_in_controller = true;
  t_paused = true;
  __imp__sub_8754BF00(ctx, base);
  t_in_controller = false;
  const bool turbo = Effective(cheats::Cheat::Turbo) || Effective(cheats::Cheat::TurboStrong);
  const bool spin = Effective(cheats::Cheat::SpinAttack);
  if (!(turbo || spin) || !ValidGuest(track)) {
    return;
  }
  const uint32_t vars = LoadGuest32(base, track + kControllerVars);
  if (!ValidGuest(vars)) {
    return;
  }
  const uint32_t joystick = LoadGuest32(base, vars + kControllerJoystick);
  const uint32_t entity = LoadGuest32(base, vars + kControllerEntity);
  const bool driving = LoadGuest32(base, vars + kControllerDisabled) == 0 && entity != 0 && !t_paused;
  const bool playing = driving && !InOnlineSession(ctx, base);
  if (turbo) {
    PublishController(joystick, playing);
  }
  if (spin && playing) {
    RequestSpinAttack(ctx, base, joystick, entity);
  }
}

// Entity_Notify(this, stock&) of each enemy and boss class: where a hit's damage is taken, before the class applies it.
#define TMNT_DAMAGE_RECEIVER(name)  \
  REX_EXTERN(__imp__##name);        \
  REX_HOOK_RAW(name) {              \
    ScalePlayerDamage(ctx, base);   \
    ++t_damage_receiver;            \
    __imp__##name(ctx, base);       \
    --t_damage_receiver;            \
  }

TMNT_DAMAGE_RECEIVER(sub_87633B98)  // FootSoldier (every Foot variant)
TMNT_DAMAGE_RECEIVER(sub_8765E8D0)  // CharacterBehaviorBase
TMNT_DAMAGE_RECEIVER(sub_87652618)  // RockSoldier
TMNT_DAMAGE_RECEIVER(sub_8763EC68)  // PizzaMonster
TMNT_DAMAGE_RECEIVER(sub_87641380)  // BoxingRobot
TMNT_DAMAGE_RECEIVER(sub_87667CF8)  // BoxingRobotBlue
TMNT_DAMAGE_RECEIVER(sub_87645050)  // Annihilator
TMNT_DAMAGE_RECEIVER(sub_87660E90)  // MiniCopter
TMNT_DAMAGE_RECEIVER(sub_87612018)  // Velociraptor
TMNT_DAMAGE_RECEIVER(sub_87621398)  // KrangFlyingSaucer
TMNT_DAMAGE_RECEIVER(sub_876578D0)  // Baxter
TMNT_DAMAGE_RECEIVER(sub_87664520)  // LeatherHead
TMNT_DAMAGE_RECEIVER(sub_87690610)  // MetalHead
TMNT_DAMAGE_RECEIVER(sub_8769D710)  // CementMan
TMNT_DAMAGE_RECEIVER(sub_87649680)  // Tokka
TMNT_DAMAGE_RECEIVER(sub_876A38E8)  // Rahzar
TMNT_DAMAGE_RECEIVER(sub_876A8068)  // SuperKrangBoss
TMNT_DAMAGE_RECEIVER(sub_87696890)  // Shredder
