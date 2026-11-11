#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "guest_session_tmnt_r.h"
#include "snes_gameplay/online_rules.h"
#include "snes_gameplay/session.h"

using namespace tmnt;

REX_EXTERN(sub_8767F968);
REX_EXTERN(sub_8767FD68);

namespace {

// Turtle script variables: *(script + 64) in script functions, *(script + 184) in compiled tracks.
constexpr uint32_t kFunctionVars = 64;
constexpr uint32_t kTrackVars = 184;
constexpr uint32_t kSnapLeftRight = 932;
constexpr uint32_t kSlamProb = 956;
constexpr uint32_t kThrowProb = 960;
constexpr uint32_t kFrontKickProb = 964;
constexpr uint32_t kUseSurfBoard = 2828;
constexpr uint32_t kUseHoverBoard = 2832;
constexpr uint32_t kDir = 3336;
constexpr uint32_t kLastFacing = 3384;
constexpr uint32_t kTrackPhase = 3876;
constexpr uint32_t kTrackUpdate = 3;

// TJC_Main's controller variables at *(track + 36).
constexpr uint32_t kControllerVars = 36;
constexpr uint32_t kControllerJoystick = 0;
constexpr uint32_t kControllerEntity = 4;

constexpr uint32_t kState = 3256;

constexpr uint32_t kImportTimeGetDt = 1433;
constexpr uint32_t kImportObjectModifier = 1855;
constexpr uint32_t kImportModifierScript = 11;
constexpr uint32_t kTurtleEntityNotify = 0x87677CF8;

struct Controller {
  uint32_t entity = 0;
  uint32_t vars = 0;
  snes::StickState stick;
};

// The turtle scripts all run on the game's script thread.
std::array<Controller, snes::kControllers> g_controllers;
std::atomic<bool> g_online_logged{false};
std::atomic<bool> g_other_turtle_logged{false};
thread_local bool t_movement_facing = false;
thread_local bool t_slam_if_throw_fails = false;

bool ValidGuest(uint32_t address) { return address >= 0x10000 && address < 0xFFFFF000; }

uint32_t Load32(uint8_t* base, uint32_t address) { return LoadBE32(GuestHost(base, address), 0); }

void Store32(uint8_t* base, uint32_t address, uint32_t value) { StoreBE32(GuestHost(base, address), 0, value); }

float LoadFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  Store32(base, address, bits);
}

float FloatBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

const std::optional<snes::RuntimeData>& Data() {
  static const std::optional<snes::RuntimeData> data =
      snes::DecodeRuntimeData(rex::cvar::Query<std::string>(snes::kRuntimeDataSetting));
  return data;
}

bool Online(PPCContext& ctx, uint8_t* base) {
  const bool online = GuestInOnlineSession(ctx, base);
  if (!online) {
    g_online_logged.store(false, std::memory_order_relaxed);
  } else if (!g_online_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("SNES Gameplay: online match, using the features locked for it");
  }
  return online;
}

bool Wanted(uint32_t session, snes::Feature feature) {
  return snes::EffectiveFeature(session, feature, rex::cvar::Query<bool>(snes::InfoOf(feature).setting));
}

snes::DashMode WantedDash() {
  return snes::EffectiveDash(rex::cvar::Query<uint32_t>(snes::kSessionSetting),
                             snes::ParseDashSetting(rex::cvar::Query<std::string>(snes::kDashSetting)));
}

// Offline a feature follows the F9 value at once. Online it follows the bits locked for this PC's seat, and only for the
// turtle this PC controls: the game's replication master decides each turtle, so the others follow their owners.
bool Active(PPCContext& ctx, uint8_t* base, snes::Feature feature, bool own_turtle = true) {
  const bool wanted = Wanted(rex::cvar::Query<uint32_t>(snes::kSessionSetting), feature);
  const bool locked_on = snes::LockedOn(rex::cvar::Query<uint32_t>(snes::kOnlineLockSetting), feature);
  const bool ready = Data().has_value();
  const bool online = (wanted || locked_on) && ready && Online(ctx, base);
  if (online && !own_turtle && locked_on && !g_other_turtle_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("SNES Gameplay: another player's turtle asked for Directional Combat; its owner decides it");
  }
  return snes::MechanicActive(wanted, ready, online, locked_on && own_turtle);
}

// The Dash mode in force: offline the F9 or saved mode at once, online the mode locked for this PC's seat.
snes::DashMode ActiveDash(PPCContext& ctx, uint8_t* base) {
  const snes::DashMode wanted = WantedDash();
  const snes::DashMode locked = snes::LockedDash(rex::cvar::Query<uint32_t>(snes::kOnlineLockSetting));
  const bool ready = Data().has_value();
  const bool online = (wanted != snes::DashMode::kOff || locked != snes::DashMode::kOff) && ready && Online(ctx, base);
  return snes::DashActive(wanted, ready, online, locked);
}

// A feature switched off drops what it kept for each controller, so it never resumes from a stale count.
void DropSwitchedOffState(PPCContext& ctx, uint8_t* base) {
  static snes::EffectiveChanges changes;
  uint32_t effective = snes::DashBits(ActiveDash(ctx, base));
  for (const snes::FeatureInfo& info : snes::kFeatures) {
    if (info.feature != snes::Feature::kDash) {
      effective |= info.gameplay && Active(ctx, base, info.feature) ? snes::FeatureBit(info.feature) : 0;
    }
  }
  if (const uint32_t changed = changes.Observe(effective)) {
    for (Controller& controller : g_controllers) {
      snes::ResetSwitchedOff(controller.stick, changed, effective);
    }
  }
}

bool OnBoard(uint8_t* base, uint32_t vars) {
  return Load32(base, vars + kUseSurfBoard) != 0 || Load32(base, vars + kUseHoverBoard) != 0;
}

uint32_t FunctionVars(uint8_t* base, uint32_t script) {
  if (!ValidGuest(script)) {
    return 0;
  }
  const uint32_t vars = Load32(base, script + kFunctionVars);
  return ValidGuest(vars) ? vars : 0;
}

// Side calls, so the hooked function's own registers stay as the game passed them.
double FrameTime(PPCContext& parent, uint8_t* base) {
  rex::CallFrame frame(parent);
  frame.ctx.lr = parent.lr;
  PPCFunc* get_dt = ResolveGuestImport(frame, base, kImportTimeGetDt);
  if (!get_dt) {
    return 0.0;
  }
  get_dt(frame.ctx, base);
  return frame.ctx.f1.f64 > 0.0 && frame.ctx.f1.f64 < 1.0 ? frame.ctx.f1.f64 : 0.0;
}

uint32_t TurtleVarsOf(PPCContext& parent, uint8_t* base, uint32_t object) {
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
  PPCFunc* modifier_script = ValidGuest(modifier) ? ResolveGuestImport(frame, base, kImportModifierScript) : nullptr;
  if (!modifier_script) {
    return 0;
  }
  frame.ctx.r3.u64 = modifier;
  modifier_script(frame.ctx, base);
  const uint32_t script = frame.ctx.r3.u32;
  if (!ValidGuest(script)) {
    return 0;
  }
  const uint32_t vtable = Load32(base, script);
  return ValidGuest(vtable) && Load32(base, vtable + 8) == kTurtleEntityNotify ? FunctionVars(base, script) : 0;
}

// The controller a TJC notification belongs to, with the turtle it drives.
Controller* ControllerOf(PPCContext& ctx, uint8_t* base, uint32_t track) {
  if (!Data() || !ValidGuest(track)) {
    return nullptr;
  }
  DropSwitchedOffState(ctx, base);
  const uint32_t vars = Load32(base, track + kControllerVars);
  if (!ValidGuest(vars)) {
    return nullptr;
  }
  const uint32_t joystick = Load32(base, vars + kControllerJoystick);
  if (joystick >= g_controllers.size()) {
    return nullptr;
  }
  Controller& controller = g_controllers[joystick];
  const uint32_t entity = Load32(base, vars + kControllerEntity);
  if (entity != controller.entity || (entity && !controller.vars)) {
    controller = Controller{};
    controller.entity = entity;
    controller.vars = entity ? TurtleVarsOf(ctx, base, entity) : 0;
  }
  return controller.vars ? &controller : nullptr;
}

Controller* ControllerFor(uint32_t vars) {
  for (Controller& controller : g_controllers) {
    if (controller.vars == vars && vars != 0) {
      return &controller;
    }
  }
  return nullptr;
}

// TJC_Notify*(this, dir): the vector arrives by value, x and y in r4, z in the high word of r5.
int DirectionOf(const PPCContext& ctx) {
  return snes::SnesDirection(FloatBits(uint32_t(ctx.r4.u64 >> 32)), FloatBits(ctx.r4.u32));
}

}  // namespace

// TJC_NotifyPlayerIdle(this): the stick is centred, or the game is paused or the controller disabled, where no time passes.
// A waiting Manual tap does not survive that, so a pause never completes a double tap.
REX_EXTERN(__imp__sub_8754B540);
REX_HOOK_RAW(sub_8754B540) {
  if (Controller* controller = ControllerOf(ctx, base, ctx.r3.u32)) {
    snes::ReleaseStick(controller->stick);
    if (snes::ManualTapWaiting(controller->stick) || controller->stick.manual.dashing) {
      const double dt = FrameTime(ctx, base);
      if (dt > 0.0) {
        snes::ManualCentre(controller->stick, dt, *Data());
      } else {
        controller->stick.manual = snes::ManualDash{};
      }
    } else {
      snes::ManualCentre(controller->stick, 0.0, *Data());
    }
  }
  __imp__sub_8754B540(ctx, base);
}

// TJC_NotifyPlayerWalk(this, dir): a partly pushed stick.
REX_EXTERN(__imp__sub_8754B318);
REX_HOOK_RAW(sub_8754B318) {
  if (Controller* controller = ControllerOf(ctx, base, ctx.r3.u32)) {
    snes::HoldStick(controller->stick, DirectionOf(ctx), FrameTime(ctx, base));
    snes::ManualBreak(controller->stick);
  }
  __imp__sub_8754B318(ctx, base);
}

// TJC_NotifyPlayerRun(this, dir): a fully pushed stick or the d-pad, which runs at once in Re-Shelled. The SNES walks
// until its dash starts, so the turtle walks until then: after a hold in Auto, after a double tap in Manual.
REX_EXTERN(__imp__sub_8754B430);
REX_HOOK_RAW(sub_8754B430) {
  Controller* controller = ControllerOf(ctx, base, ctx.r3.u32);
  if (controller) {
    const int direction = DirectionOf(ctx);
    const double dt = FrameTime(ctx, base);
    bool dashing = snes::PushStick(controller->stick, direction, dt, *Data());
    const snes::DashMode mode = ActiveDash(ctx, base);
    if (mode == snes::DashMode::kManual) {
      dashing = snes::ManualPush(controller->stick, direction, dt, *Data());
    }
    if (!dashing && !OnBoard(base, controller->vars) && mode != snes::DashMode::kOff) {
      __imp__sub_8754B318(ctx, base);
      return;
    }
  }
  __imp__sub_8754B430(ctx, base);
}

// Turtle_SetWalkAnimation(this) and Turtle_SetRunAnimation(this): the moves that ask Turtle_CalcFacing every frame.
REX_EXTERN(__imp__sub_87679280);
REX_HOOK_RAW(sub_87679280) {
  const bool outer = t_movement_facing;
  t_movement_facing = true;
  __imp__sub_87679280(ctx, base);
  t_movement_facing = outer;
}

REX_EXTERN(__imp__sub_8767AE70);
REX_HOOK_RAW(sub_8767AE70) {
  const bool outer = t_movement_facing;
  t_movement_facing = true;
  __imp__sub_8767AE70(ctx, base);
  t_movement_facing = outer;
}

// Turtle_RequestJump(this, forced): a request made in the air is Re-Shelled's double jump. Forced jumps, the first jump,
// bounces and scripted jumps do not come through that case.
REX_EXTERN(__imp__sub_876789C0);
REX_HOOK_RAW(sub_876789C0) {
  const uint32_t vars = FunctionVars(base, ctx.r3.u32);
  if (vars && snes::BlocksAirJump(int(Load32(base, vars + kState)), ctx.r4.u32 != 0, OnBoard(base, vars)) &&
      ControllerFor(vars) && Active(ctx, base, snes::Feature::kDoubleJump)) {
    ctx.r3.s64 = -1;
    return;
  }
  __imp__sub_876789C0(ctx, base);
}

// Turtle_CalcFacing(this): m_LastFacing from the body's sight with a 95 degree hysteresis. The SNES (80:B370) turns
// only when the pad has a horizontal part, so walking up or down keeps the side the turtle fights on.
REX_EXTERN(__imp__sub_8767CA70);
REX_HOOK_RAW(sub_8767CA70) {
  const bool moving = t_movement_facing;
  const uint32_t vars = FunctionVars(base, ctx.r3.u32);
  const uint32_t before = vars ? Load32(base, vars + kLastFacing) : 0;
  __imp__sub_8767CA70(ctx, base);
  if (!moving || !vars || (before != snes::kLeft && before != snes::kRight) || OnBoard(base, vars) ||
      !Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)) {
    return;
  }
  const int direction = snes::SnesDirection(LoadFloat(base, vars + kDir), LoadFloat(base, vars + kDir + 4));
  Store32(base, vars + kLastFacing, uint32_t(snes::NextFacing(int(before), direction, *Data())));
}

// Turtle_SnapLeftRight(this): attacks, idles and recoveries face left or right only when the turtle's m_SnapLeftRight
// asks for it. SNES attacks always strike along the facing side.
REX_EXTERN(__imp__sub_8767C730);
REX_HOOK_RAW(sub_8767C730) {
  const uint32_t vars = FunctionVars(base, ctx.r3.u32);
  if (!vars || OnBoard(base, vars) ||
      !Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)) {
    __imp__sub_8767C730(ctx, base);
    return;
  }
  const uint32_t saved = Load32(base, vars + kSnapLeftRight);
  Store32(base, vars + kSnapLeftRight, 1);
  __imp__sub_8767C730(ctx, base);
  Store32(base, vars + kSnapLeftRight, saved);
}

// 100_Turtle_PreAttack: each update draws MTH_FloatRand against P(slam), P(throw) and P(front kick). The SNES picks the
// grab by input alone, so the draw is made to land on the slam or the throw the stick asks for; without a grab target
// both give the normal attack, and a lone enemy behind still gets the back kick first.
REX_EXTERN(__imp__sub_8768BB80);
REX_HOOK_RAW(sub_8768BB80) {
  const uint32_t script = ctx.r3.u32;
  const uint32_t track_vars = ValidGuest(script) ? Load32(base, script + kTrackVars) : 0;
  const uint32_t vars = FunctionVars(base, script);
  const Controller* controller = vars ? ControllerFor(vars) : nullptr;
  if (!ValidGuest(track_vars) || Load32(base, track_vars + kTrackPhase) != kTrackUpdate || !controller ||
      !Active(ctx, base, snes::Feature::kThrowControl)) {
    __imp__sub_8768BB80(ctx, base);
    return;
  }
  const int facing = int(Load32(base, vars + kLastFacing));
  const bool throw_to_screen =
      snes::ChooseGrab(controller->stick, facing, *Data()) == snes::GrabChoice::kThrowToScreen;
  const float slam = LoadFloat(base, track_vars + kSlamProb);
  const float throw_prob = LoadFloat(base, track_vars + kThrowProb);
  const float front_kick = LoadFloat(base, track_vars + kFrontKickProb);
  StoreFloat(base, track_vars + kSlamProb, throw_to_screen ? -1.0f : 2.0f);
  StoreFloat(base, track_vars + kThrowProb, throw_to_screen ? 3.0f : 0.0f);
  StoreFloat(base, track_vars + kFrontKickProb, 0.0f);
  t_slam_if_throw_fails = throw_to_screen;
  __imp__sub_8768BB80(ctx, base);
  t_slam_if_throw_fails = false;
  StoreFloat(base, track_vars + kSlamProb, slam);
  StoreFloat(base, track_vars + kThrowProb, throw_prob);
  StoreFloat(base, track_vars + kFrontKickProb, front_kick);
}

// Turtle_RequestThrowAttack(this, &target): a target that refuses the screen throw is slammed, as the SNES slams every
// grab it cannot throw. A target that refuses both keeps the game's normal attack.
REX_EXTERN(__imp__sub_87683B20);
REX_HOOK_RAW(sub_87683B20) {
  const bool slam_instead = t_slam_if_throw_fails;
  t_slam_if_throw_fails = false;
  const uint32_t script = ctx.r3.u32;
  const uint32_t target = ctx.r4.u32;
  __imp__sub_87683B20(ctx, base);
  if (!slam_instead || ctx.r3.s32 == 0) {
    return;
  }
  rex::CallFrame frame(ctx);
  frame.ctx.lr = ctx.lr;
  frame.ctx.r3.u64 = script;
  sub_8767F968(frame.ctx, base);
  const uint32_t slam_dir = frame.ctx.r3.u32;
  frame.ctx.r3.u64 = script;
  frame.ctx.r4.u64 = target;
  frame.ctx.r5.u64 = slam_dir;
  frame.ctx.r6.u64 = 0;
  sub_8767FD68(frame.ctx, base);
  if (frame.ctx.r3.s32 == 0) {
    ctx.r3.u64 = 0;
  }
}
