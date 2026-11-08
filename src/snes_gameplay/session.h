#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cheats.h"
#include "snes_gameplay/rules.h"

namespace tmnt::snes {

// The F9 menu's overrides, written by default.xex and read by TMNT_r. Never saved.
inline constexpr char kSessionSetting[] = "tmnt_snes_session";

enum class Feature : uint8_t { kClassicSoundtrack, kDirectionalCombat, kThrowControl, kDash };
inline constexpr size_t kFeatureCount = 4;

struct FeatureInfo {
  Feature feature;
  const char* setting;
  const char* label;
  bool gameplay;
};

inline constexpr std::array<FeatureInfo, kFeatureCount> kFeatures{{
    {Feature::kClassicSoundtrack, "tmnt_classic_soundtrack", "Classic Soundtrack", false},
    {Feature::kDirectionalCombat, kDirectionalCombatSetting, "Directional Combat", true},
    {Feature::kThrowControl, kThrowControlSetting, "SNES Throw Control", true},
    {Feature::kDash, kDashSetting, "SNES Dash", true},
}};

inline const FeatureInfo& InfoOf(Feature feature) { return kFeatures[static_cast<size_t>(feature)]; }

// Two bits per feature, so the two modules share the overrides as one cvar.
inline cheats::Override OverrideOf(uint32_t packed, Feature feature) {
  return static_cast<cheats::Override>((packed >> (static_cast<uint32_t>(feature) * 2)) & 3);
}

// The session override if there is one, otherwise the launcher's saved value.
inline bool EffectiveFeature(uint32_t packed, Feature feature, bool saved) {
  return cheats::EffectiveCheat(saved, OverrideOf(packed, feature));
}

// An override equal to the saved value is dropped, so a feature toggled back follows the saved value again.
inline uint32_t WithOverride(uint32_t packed, Feature feature, bool saved, bool enabled) {
  const uint32_t shift = static_cast<uint32_t>(feature) * 2;
  const cheats::Override next = enabled == saved ? cheats::Override::kFollowsSaved
                                                 : (enabled ? cheats::Override::kOn : cheats::Override::kOff);
  return (packed & ~(uint32_t(3) << shift)) | (static_cast<uint32_t>(next) << shift);
}

inline uint32_t ToggledOverride(uint32_t packed, Feature feature, bool saved) {
  return WithOverride(packed, feature, saved, !EffectiveFeature(packed, feature, saved));
}

// The features whose effective value changed since the last call, one bit each. The first call only takes a baseline.
class EffectiveChanges {
 public:
  uint32_t Observe(uint32_t effective) {
    const uint32_t changed = seen_ ? effective ^ last_ : 0;
    last_ = effective;
    seen_ = true;
    return changed;
  }

 private:
  uint32_t last_ = 0;
  bool seen_ = false;
};

inline uint32_t FeatureBit(Feature feature) { return uint32_t(1) << static_cast<uint32_t>(feature); }

// Applies a feature's reset to a controller's stick when the feature is among the changed ones and now off.
inline void ResetSwitchedOff(StickState& stick, uint32_t changed, uint32_t effective) {
  const uint32_t off = changed & ~effective;
  if (off & FeatureBit(Feature::kDash)) ResetDashState(stick);
  if (off & FeatureBit(Feature::kThrowControl)) ResetThrowState(stick);
}

}  // namespace tmnt::snes
