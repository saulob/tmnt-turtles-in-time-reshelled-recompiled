#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "online/snes_features.h"
#include "snes_gameplay/session.h"

namespace tmnt::snes {

// The online mechanics are the gameplay features in their menu order, without Classic Soundtrack.
static_assert(uint32_t(Feature::kDirectionalCombat) == uint32_t(online::SnesFeature::kDirectionalCombat) + 1 &&
              uint32_t(Feature::kThrowControl) == uint32_t(online::SnesFeature::kThrowControl) + 1 &&
              uint32_t(Feature::kDash) == uint32_t(online::SnesFeature::kDash) + 1);

inline std::optional<online::SnesFeature> OnlineFeature(Feature feature) {
  if (!InfoOf(feature).gameplay) return std::nullopt;
  return online::SnesFeatureAt(int(feature) - 1);
}

// The lock the exe publishes for the game module: bit 3 says a match has started, bits 0-2 are its mechanics.
inline constexpr char kOnlineLockSetting[] = "tmnt_snes_online_lock";
inline constexpr uint32_t kOnlineLockedFlag = 0x8;

inline uint32_t EncodeOnlineLock(const std::optional<online::SnesGameplaySet>& locked) {
  return locked ? kOnlineLockedFlag | locked->Bits() : 0;
}

inline bool LockedOn(uint32_t lock, Feature feature) {
  const std::optional<online::SnesFeature> online = OnlineFeature(feature);
  return online && (lock & kOnlineLockedFlag) != 0 && (lock & (1u << uint32_t(*online))) != 0;
}

// Offline a mechanic follows the player's own choice at once; online only what the match locked, all Off before that.
inline bool MechanicActive(bool wanted, bool data_ready, bool online, bool locked_on) {
  return data_ready && (online ? locked_on : wanted);
}

inline uint32_t CompatId(std::string_view text) {
  uint32_t hash = 2166136261u;
  for (const char c : text) {
    hash = (hash ^ uint8_t(c)) * 16777619u;
  }
  return hash == 0 || hash == online::kSnesCompatMixed ? 1u : hash;
}

// Only what each mechanic reads: the facing table, and the hold times in seconds, which depend on the 50 or 60 Hz clock
// of the imported ROM.
inline uint32_t GameplayCompat(Feature feature, const RuntimeData& data) {
  std::string facing;
  for (const Facing f : data.facing) facing += FacingChar(f);
  char rate[32];
  std::snprintf(rate, sizeof rate, "%.4f", data.frame_rate);
  switch (feature) {
    case Feature::kDirectionalCombat: return CompatId("directional:1:" + facing);
    case Feature::kThrowControl:
      return CompatId("throw:1:" + facing + ":" + rate + ":" + std::to_string(data.throw_hold_frames));
    case Feature::kDash: return CompatId(std::string("dash:1:") + rate + ":" + std::to_string(data.dash_hold_frames));
    case Feature::kClassicSoundtrack: break;
  }
  return 0;
}

}  // namespace tmnt::snes
