#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tmnt::cheats {

inline constexpr char kInfiniteHealthSetting[] = "tmnt_cheat_infinite_health";
inline constexpr char kInfiniteLivesSetting[] = "tmnt_cheat_infinite_lives";
inline constexpr char kTurboAttackSetting[] = "tmnt_turbo_attack";
inline constexpr char kTurboStrongAttackSetting[] = "tmnt_turbo_strong_attack";
inline constexpr char kDamageMultiplierSetting[] = "tmnt_cheat_damage_multiplier";
inline constexpr char kSuperArmorSetting[] = "tmnt_cheat_super_armor";
inline constexpr char kSpinAttackSetting[] = "tmnt_cheat_spin_attack";
// Written by TMNT_r, read by the pad hook in default.xex: the two modules share only the cvar registry.
inline constexpr char kTurboGameplaySetting[] = "tmnt_turbo_gameplay";
// The F10 menu's overrides, written by default.xex and read by TMNT_r. Never saved.
inline constexpr char kSessionSetting[] = "tmnt_cheat_session";

inline constexpr size_t kControllers = 4;

enum class Cheat : uint8_t { Health, Lives, Turbo, TurboStrong, SuperArmor, SpinAttack };
inline constexpr size_t kCheatCount = 6;

struct CheatInfo {
  Cheat cheat;
  const char* setting;
  const char* label;
};

inline constexpr std::array<CheatInfo, kCheatCount> kCheats{{
    {Cheat::Health, kInfiniteHealthSetting, "Infinite Health"},
    {Cheat::Lives, kInfiniteLivesSetting, "Infinite Lives"},
    {Cheat::Turbo, kTurboAttackSetting, "Turbo Attack"},
    {Cheat::TurboStrong, kTurboStrongAttackSetting, "Turbo Strong Attack"},
    {Cheat::SuperArmor, kSuperArmorSetting, "Super Armor"},
    {Cheat::SpinAttack, kSpinAttackSetting, "Spin Attack"},
}};

enum class Override : uint8_t { kFollowsSaved = 0, kOff = 1, kOn = 2 };

// The session overrides of all cheats in one value, two bits each, so the two modules share them as one cvar.
inline Override OverrideOf(uint32_t packed, Cheat cheat) {
  return static_cast<Override>((packed >> (static_cast<uint32_t>(cheat) * 2)) & 3);
}

inline bool EffectiveCheat(bool saved, Override override_state) {
  return override_state == Override::kFollowsSaved ? saved : override_state == Override::kOn;
}

inline bool EffectiveCheat(uint32_t packed, Cheat cheat, bool saved) {
  return EffectiveCheat(saved, OverrideOf(packed, cheat));
}

// An override equal to the saved value is dropped, so a cheat toggled back follows the saved value again.
inline uint32_t WithOverride(uint32_t packed, Cheat cheat, bool saved, bool enabled) {
  const uint32_t shift = static_cast<uint32_t>(cheat) * 2;
  const Override next = enabled == saved ? Override::kFollowsSaved : (enabled ? Override::kOn : Override::kOff);
  return (packed & ~(uint32_t(3) << shift)) | (static_cast<uint32_t>(next) << shift);
}

inline uint32_t ToggledOverride(uint32_t packed, Cheat cheat, bool saved) {
  return WithOverride(packed, cheat, saved, !EffectiveCheat(packed, cheat, saved));
}

// The saved Damage Multiplier: 0 Off, 2 or 4; anything else is Off.
inline int ParseDamageMultiplier(int value) { return value == 2 || value == 4 ? value : 0; }

inline int NextDamageMultiplier(int multiplier) {
  return multiplier == 0 ? 2 : (multiplier == 2 ? 4 : 0);
}

// The multiplier override sits above the toggles: 0 follows the saved value, 1 Off, 2 2x, 3 4x.
inline constexpr uint32_t kMultiplierShift = 16;
static_assert(kCheatCount * 2 <= kMultiplierShift);

inline int EffectiveMultiplier(uint32_t packed, int saved) {
  switch ((packed >> kMultiplierShift) & 3) {
    case 1:
      return 0;
    case 2:
      return 2;
    case 3:
      return 4;
    default:
      return ParseDamageMultiplier(saved);
  }
}

inline uint32_t WithMultiplier(uint32_t packed, int saved, int multiplier) {
  multiplier = ParseDamageMultiplier(multiplier);
  const uint32_t code =
      multiplier == ParseDamageMultiplier(saved) ? 0 : (multiplier == 0 ? 1 : (multiplier == 2 ? 2 : 3));
  return (packed & ~(uint32_t(3) << kMultiplierShift)) | (code << kMultiplierShift);
}

inline uint32_t CycledMultiplier(uint32_t packed, int saved) {
  return WithMultiplier(packed, saved, NextDamageMultiplier(EffectiveMultiplier(packed, saved)));
}

// Entity note types (Entity.h) that carry a hit's damage in f32_1 and its source in po_Obj1.
inline constexpr uint32_t kEntityHit = 3;
inline constexpr uint32_t kEntityGetSlammed = 22;

inline bool ScalesDamage(uint32_t note, bool from_player_turtle) {
  return from_player_turtle && (note == kEntityHit || note == kEntityGetSlammed);
}

// Positive, finite damage times the multiplier, kept finite; anything else is left for the game to handle.
inline float ScaledDamage(float damage, int multiplier) {
  if (multiplier <= 1 || !std::isfinite(damage) || damage <= 0.0f) {
    return damage;
  }
  const float scaled = damage * static_cast<float>(multiplier);
  return std::isfinite(scaled) ? scaled : std::numeric_limits<float>::max();
}

// Turtle m_State values (Entity.h) whose hit handling stays the game's own.
inline constexpr uint32_t kEntityDying = 4;
inline constexpr uint32_t kEntityGrabbed = 32;
inline constexpr uint32_t kEntityGrabbedHit = 36;

// Hit types (AttackTypes_DEF.h).
inline constexpr uint32_t kHitLight = 0;
inline constexpr uint32_t kHitStrong = 1;
inline constexpr uint32_t kHitFlattened = 3;
inline constexpr uint32_t kHitFullSpinPowerUp = 6;
inline constexpr uint32_t kHitSlamAcquire = 11;
inline constexpr uint32_t kHitSewerTrap = 13;
inline constexpr uint32_t kHitBubbleKrang = 16;

// Every hit Turtle_RequestHitRouting reacts to is a combat or hazard reaction, except the Spinning Pizza pick-up (routed
// before any damage) and the turtle's own slam detector.
inline bool SuperArmorCovers(uint32_t hit_type) {
  return hit_type != kHitFullSpinPowerUp && hit_type != kHitSlamAcquire;
}

inline bool AliveAndFree(float health, uint32_t state) {
  return std::isfinite(health) && health > 0.0f && state != kEntityDying && state != kEntityGrabbed &&
         state != kEntityGrabbedHit;
}

// Turtle_RequestHitRouting has already applied the damage when it asks Turtle_IsNoHitReact and then
// Turtle_IsUltimateHit. Answering yes and then no ends the hit there: the state stays (an attack goes on) and none of
// the hit, concuss, flatten, bubble and special-hit tracks that carry the knockback is entered. A hit that empties the
// health keeps the game's path to the death track.
inline bool SuperArmorAbsorbs(bool enabled, float health, uint32_t state, uint32_t hit_type) {
  return enabled && SuperArmorCovers(hit_type) && AliveAndFree(health, state);
}

// Turtle_RequestGrabbed refuses with -1 when the turtle is immune, before any state change; the grabbing enemy takes a
// bone of 0 or less as a missed grab.
inline constexpr int32_t kGrabRefused = -1;

inline bool SuperArmorRefusesGrab(bool enabled, float health, uint32_t state) {
  return enabled && AliveAndFree(health, state);
}

// XInput B (0x2000), IO_JoyButton_XBox_B in default.xex's remap table. Turtles do nothing with it in play.
inline constexpr uint32_t kSpinAttackIoButton = 2;

inline constexpr uint32_t kEntityIdle = 0;
inline constexpr uint32_t kEntityAttack = 2;
inline constexpr uint32_t kEntityWalk = 9;
inline constexpr uint32_t kEntityRun = 11;
inline constexpr uint32_t kEntityAttackFullSpin = 35;
inline constexpr uint32_t kEntityAttackSlide = 42;
inline constexpr uint32_t kEntityAttackRecovery = 65;

// The grounded, player-driven states a Spin Attack may start from; a spin already running is never restarted.
inline bool SpinAttackAllowed(bool enabled, bool just_pressed, float health, uint32_t state) {
  if (!enabled || !just_pressed || !std::isfinite(health) || health <= 0.0f) {
    return false;
  }
  switch (state) {
    case kEntityIdle:
    case kEntityAttack:
    case kEntityWalk:
    case kEntityRun:
    case kEntityAttackSlide:
    case kEntityAttackRecovery:
      return true;
    default:
      return false;
  }
}

// XInput X. default.xex maps it to IO_JoyButton_XBox_X, which TJC_Main turns into TJC_NotifyPlayerAttack.
inline constexpr uint16_t kTurboAttackButton = 0x4000;
// XInput Y, IO_JoyButton_XBox_Y: TJC_NotifyPlayerStrongAttack.
inline constexpr uint16_t kTurboStrongAttackButton = 0x8000;

// Half of the turbo press interval: a fresh press every 50 ms while the button stays held.
inline constexpr double kTurboPulseSeconds = 0.025;

inline bool CheatActive(bool saved, bool online) { return saved && !online; }

// Cheats change the simulation both players run, so they are off from the moment Online play is enabled, not only in a
// match; a running online game session blocks them too.
inline bool CheatsAllowed(bool online_enabled, bool in_online_session = false) {
  return !online_enabled && !in_online_session;
}

inline constexpr char kOnlineLockoutText[] = "Cheats are disabled while Online Play is enabled";

// Turtle_TakeDamage's health damage. A replicated death empties the health through the same call, so it is kept.
inline float ShieldedDamage(float damage, bool enabled, bool death) {
  return enabled && !death && damage > 0.0f ? 0.0f : damage;
}

// What brings a living, damaged turtle back to its maximum; 0 for a full or dead one.
inline float RefillAmount(float health, float max, bool enabled) {
  if (!enabled || !std::isfinite(health) || !std::isfinite(max) || max <= 0.0f || health <= 0.0f || health >= max) {
    return 0.0f;
  }
  return max - health;
}

// BasicWorld_PlayerRemoved respawns while its read of the slot's lives, minus one, stays above zero. The read is one
// higher while the loss is dropped, so the respawn follows the count the HUD keeps. An empty slot stays empty.
inline int32_t LivesSeenByRemoval(int32_t lives, bool armed) { return armed && lives >= 1 ? lives + 1 : lives; }

// PlayerProfile_RegisterLives' delta: a loss is dropped, a gain passes.
inline int32_t LivesDelta(int32_t delta, bool armed) { return armed && delta < 0 ? 0 : delta; }

// tmnt_turbo_gameplay holds 16 bits per joystick: 0 while that joystick drives no turtle in unpaused play, otherwise a
// count that moves on every controller tick.
inline uint64_t WithControllerTick(uint64_t gate, uint32_t joystick, bool driving) {
  if (joystick >= kControllers) {
    return gate;
  }
  const uint32_t shift = joystick * 16;
  uint64_t count = (gate >> shift) & 0xFFFF;
  count = driving ? (count == 0xFFFF ? 1 : count + 1) : 0;
  return (gate & ~(uint64_t(0xFFFF) << shift)) | (count << shift);
}

// The pad poll's reading of tmnt_turbo_gameplay. A count that stops moving belongs to a controller the game destroyed
// without a last tick, so it no longer counts as play.
class GameplayGate {
 public:
  static constexpr double kStaleSeconds = 0.2;

  // Joysticks driving a turtle now, one bit each. Call on every poll so a stopped count is noticed.
  uint32_t Observe(uint64_t gate, double now) {
    uint32_t driving = 0;
    for (uint32_t joystick = 0; joystick < kControllers; ++joystick) {
      const uint16_t count = static_cast<uint16_t>(gate >> (joystick * 16));
      if (count != seen_[joystick]) {
        seen_[joystick] = count;
        moved_[joystick] = now;
      }
      if (count != 0 && now - moved_[joystick] <= kStaleSeconds) {
        driving |= 1u << joystick;
      }
    }
    return driving;
  }

 private:
  std::array<uint16_t, kControllers> seen_{};
  std::array<double, kControllers> moved_{};
};

// Turns one held attack button into fresh press edges. The guest still decides whether each press becomes an attack;
// this only rewrites the button bit it sees.
class TurboAttackState {
 public:
  explicit TurboAttackState(uint16_t button = kTurboAttackButton) : button_(button) {}

  // The buttons the guest should see for this poll. `now` is a monotonic host time in seconds.
  uint16_t Apply(uint16_t buttons, bool enabled, bool gameplay, double now) {
    const bool physical = (buttons & button_) != 0;
    if (!enabled || !gameplay || !physical) {
      holding_ = false;
      return buttons;
    }
    if (!holding_) {
      holding_ = true;
      engaged_ = true;
      pulses_ = 1;
      next_edge_ = now + kTurboPulseSeconds;
      return buttons;
    }
    for (int step = 0; step < 4 && now >= next_edge_; ++step) {
      engaged_ = !engaged_;
      next_edge_ += kTurboPulseSeconds;
      if (engaged_) {
        ++pulses_;
      }
    }
    return engaged_ ? buttons : static_cast<uint16_t>(buttons & ~button_);
  }

  bool holding() const { return holding_; }

  uint32_t pulses() const { return pulses_; }

 private:
  uint16_t button_;
  bool holding_ = false;
  bool engaged_ = false;
  double next_edge_ = 0.0;
  uint32_t pulses_ = 0;
};

// Defined in cheats.cpp (default.xex). The effective value is the F10 override if there is one, else the saved setting.
bool Enabled(Cheat cheat);
void ToggleForSession(Cheat cheat);
int DamageMultiplier();
void CycleDamageMultiplierForSession();

// Online play clears the session overrides and every saved cheat cvar; changing back to Off restores nothing.
void SetOnline(bool online);

// Rewrites the attack bits of a successful XamInputGetState for `user`.
void ApplyTurboAttack(uint8_t* base, uint32_t user, uint32_t state);

}  // namespace tmnt::cheats
