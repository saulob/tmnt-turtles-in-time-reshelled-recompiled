#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "online/snes_features.h"
#include "snes_gameplay/session.h"

namespace tmnt::snes {

// The lock the exe publishes for the game module: bit 7 says a match has started, bits 0-4 are its mechanics in
// online::SnesFeature order and bits 5-6 the Dash mode (Auto 1, Manual 2).
inline constexpr char kOnlineLockSetting[] = "tmnt_snes_online_lock";
inline constexpr uint32_t kOnlineLockedFlag = 0x80;
inline constexpr uint32_t kOnlineLockMechanics = 0x1F;
inline constexpr uint32_t kOnlineLockDashShift = 5;

inline uint32_t EncodeOnlineLock(const std::optional<online::SnesGameplaySet>& locked) {
  if (!locked) return 0;
  const uint32_t mode = locked->Has(online::SnesFeature::kDash) ? uint32_t(locked->dash) : 0;
  return kOnlineLockedFlag | locked->Bits() | (mode << kOnlineLockDashShift);
}

inline bool LockedOn(uint32_t lock, Feature feature) {
  const std::optional<online::SnesFeature> online = OnlineFeature(feature);
  return online && (lock & kOnlineLockedFlag) != 0 && (lock & (1u << uint32_t(*online))) != 0;
}

// The Dash mode the match locked for this player; Off without a lock or for a mode no version knows.
inline DashMode LockedDash(uint32_t lock) {
  if (!LockedOn(lock, Feature::kDash)) return DashMode::kOff;
  const uint32_t mode = (lock >> kOnlineLockDashShift) & 3;
  return mode == uint32_t(DashMode::kManual) ? DashMode::kManual : (mode == uint32_t(DashMode::kAuto) ? DashMode::kAuto : DashMode::kOff);
}

// Offline a mechanic follows the player's own choice at once; online only what the match locked, all Off before that.
inline bool MechanicActive(bool wanted, bool data_ready, bool online, bool locked_on) {
  return data_ready && (online ? locked_on : wanted);
}

inline DashMode DashActive(DashMode wanted, bool data_ready, bool online, DashMode locked) {
  return !data_ready ? DashMode::kOff : (online ? locked : wanted);
}

inline uint32_t CompatId(std::string_view text) {
  uint32_t hash = 2166136261u;
  for (const char c : text) {
    hash = (hash ^ uint8_t(c)) * 16777619u;
  }
  return hash == 0 || hash == online::kSnesCompatMixed ? 1u : hash;
}

// Only what each mechanic reads. The Dash hold and tap times in seconds depend on the 50 or 60 Hz clock of the imported
// ROM, so a PAL and an NTSC player do not share a Dash. The jump rule has no data of its own.
inline uint32_t GameplayCompat(Feature feature, const RuntimeData& data) {
  std::string facing;
  for (const Facing f : data.facing) facing += FacingChar(f);
  char rate[32];
  std::snprintf(rate, sizeof rate, "%.4f", data.frame_rate);
  switch (feature) {
    case Feature::kDirectionalCombat: return CompatId("directional:1:" + facing);
    case Feature::kThrowControl:
      return CompatId("throw:1:" + facing + ":" + rate + ":" + std::to_string(data.throw_hold_frames));
    case Feature::kDoubleJump: return CompatId("double_jump:1");
    case Feature::kDash:
      return CompatId(std::string("dash:2:off,auto,manual:") + rate + ":" + std::to_string(data.dash_hold_frames) + ":" +
                      std::to_string(data.dash_tap_frames));
    case Feature::kClassicSoundtrack: break;
  }
  return 0;
}

}  // namespace tmnt::snes
