#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "cheats.h"
#include "online/snes_features.h"
#include "snes_gameplay/rules.h"

namespace tmnt::snes {

// The F9 menu's overrides, written by default.xex and read by TMNT_r. Never saved.
inline constexpr char kSessionSetting[] = "tmnt_snes_session";

// In menu order.
enum class Feature : uint8_t {
  kClassicSoundtrack,
  kDirectionalCombat,
  kThrowControl,
  kDoubleJump,
  kDash,
};
inline constexpr size_t kFeatureCount = 5;

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
    {Feature::kDoubleJump, kDoubleJumpSetting, "Disable Double Jump", true},
    {Feature::kDash, kDashSetting, "SNES Dash", true},
}};

inline const FeatureInfo& InfoOf(Feature feature) { return kFeatures[static_cast<size_t>(feature)]; }

// The online mechanic a gameplay row negotiates; the soundtrack is local and has none.
inline std::optional<online::SnesFeature> OnlineFeature(Feature feature) {
  switch (feature) {
    case Feature::kClassicSoundtrack: break;
    case Feature::kDirectionalCombat: return online::SnesFeature::kDirectionalCombat;
    case Feature::kThrowControl: return online::SnesFeature::kThrowControl;
    case Feature::kDoubleJump: return online::SnesFeature::kDoubleJump;
    case Feature::kDash: return online::SnesFeature::kDash;
  }
  return std::nullopt;
}

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

// The Dash override takes its two bits as 0 follows the saved value, then 1 Off, 2 Auto, 3 Manual.
inline DashMode EffectiveDash(uint32_t packed, DashMode saved) {
  const uint32_t value = (packed >> (static_cast<uint32_t>(Feature::kDash) * 2)) & 3;
  return value == 0 ? saved : static_cast<DashMode>(value - 1);
}

inline uint32_t WithDash(uint32_t packed, DashMode saved, DashMode mode) {
  const uint32_t shift = static_cast<uint32_t>(Feature::kDash) * 2;
  const uint32_t value = mode == saved ? 0 : static_cast<uint32_t>(mode) + 1;
  return (packed & ~(uint32_t(3) << shift)) | (value << shift);
}

// Off, Auto, Manual in a ring; `step` is 1 for the next mode and -1 for the previous one.
inline DashMode SteppedDash(DashMode mode, int step) {
  return static_cast<DashMode>((static_cast<int>(mode) + (step < 0 ? 2 : 1)) % 3);
}

inline uint32_t SteppedDashOverride(uint32_t packed, DashMode saved, int step) {
  return WithDash(packed, saved, SteppedDash(EffectiveDash(packed, saved), step));
}

inline const char* DashLabel(DashMode mode) {
  switch (mode) {
    case DashMode::kOff: return "Off";
    case DashMode::kAuto: return "Auto";
    case DashMode::kManual: return "Manual";
  }
  return "Off";
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

// Dash is on in two modes, so the observed word carries which one next to the feature bits.
inline constexpr uint32_t kDashManualBit = uint32_t(1) << 8;

inline uint32_t DashBits(DashMode mode) {
  return (mode != DashMode::kOff ? FeatureBit(Feature::kDash) : 0) | (mode == DashMode::kManual ? kDashManualBit : 0);
}

// Applies a feature's reset to a controller's stick when it is among the changed ones: a feature switched off drops its
// state, and any change of the Dash mode drops both dash counts.
inline void ResetSwitchedOff(StickState& stick, uint32_t changed, uint32_t effective) {
  const uint32_t off = changed & ~effective;
  if (changed & (FeatureBit(Feature::kDash) | kDashManualBit)) ResetDashState(stick);
  if (off & FeatureBit(Feature::kThrowControl)) ResetThrowState(stick);
}

}  // namespace tmnt::snes
