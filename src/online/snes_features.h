#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tmnt::online {

// How a player's SNES Dash starts. Auto and Manual are one setting, never two switches.
enum class DashMode : uint8_t { kOff = 0, kAuto = 1, kManual = 2 };

constexpr const char* DashModeKey(DashMode mode) {
  switch (mode) {
    case DashMode::kOff: return "off";
    case DashMode::kAuto: return "auto";
    case DashMode::kManual: return "manual";
  }
  return "off";
}

// The mode a wire or saved text names; nothing for text no version of the game writes.
constexpr std::optional<DashMode> ParseDashModeKey(std::string_view text) {
  if (text == "off") return DashMode::kOff;
  if (text == "auto") return DashMode::kAuto;
  if (text == "manual") return DashMode::kManual;
  return std::nullopt;
}

// The SNES gameplay mechanics an online match negotiates. Classic Soundtrack is local presentation and is never here.
enum class SnesFeature : uint8_t {
  kDirectionalCombat = 0,
  kThrowControl = 1,
  kDash = 2,
  kDoubleJump = 3,
};
inline constexpr int kSnesFeatureCount = 4;
// Party slots 1 to 4, the room's seats.
inline constexpr int kSnesSeats = 4;

constexpr SnesFeature SnesFeatureAt(int index) { return SnesFeature(index); }

// The wire key. Header only: the protocol parser uses it in targets that do not link the negotiation rule.
constexpr const char* SnesKey(SnesFeature feature) {
  switch (feature) {
    case SnesFeature::kDirectionalCombat: return "directional_combat";
    case SnesFeature::kThrowControl: return "throw_control";
    case SnesFeature::kDash: return "dash";
    case SnesFeature::kDoubleJump: return "double_jump";
  }
  return "";
}

// Features an older game or service does not know. Dash itself is old; only its Manual mode is new.
constexpr bool SnesFeatureNeedsModes(SnesFeature feature) { return feature == SnesFeature::kDoubleJump; }

const char* SnesLabel(SnesFeature feature);

// A player's choices, or the mechanics a match supports. For Dash, `dash` is the mode; a supported Dash is kAuto.
struct SnesGameplaySet {
  bool directional_combat = false;
  bool throw_control = false;
  DashMode dash = DashMode::kOff;
  bool double_jump = false;

  constexpr SnesGameplaySet() = default;
  constexpr SnesGameplaySet(bool directional, bool throw_on, DashMode mode, bool jump = false)
      : directional_combat(directional), throw_control(throw_on), dash(mode), double_jump(jump) {}
  constexpr SnesGameplaySet(bool directional, bool throw_on, bool dash_on, bool jump = false)
      : SnesGameplaySet(directional, throw_on, dash_on ? DashMode::kAuto : DashMode::kOff, jump) {}

  bool Has(SnesFeature feature) const {
    switch (feature) {
      case SnesFeature::kDirectionalCombat: return directional_combat;
      case SnesFeature::kThrowControl: return throw_control;
      case SnesFeature::kDash: return dash != DashMode::kOff;
      case SnesFeature::kDoubleJump: return double_jump;
    }
    return false;
  }
  // Turning Dash on keeps a mode already chosen, otherwise it is Auto.
  void Set(SnesFeature feature, bool value) {
    switch (feature) {
      case SnesFeature::kDirectionalCombat: directional_combat = value; break;
      case SnesFeature::kThrowControl: throw_control = value; break;
      case SnesFeature::kDash:
        dash = !value ? DashMode::kOff : (dash == DashMode::kOff ? DashMode::kAuto : dash);
        break;
      case SnesFeature::kDoubleJump: double_jump = value; break;
    }
  }
  bool Any() const { return Bits() != 0; }
  // One bit per mechanic in SnesFeature order; Dash is set for either mode.
  uint8_t Bits() const {
    uint8_t bits = 0;
    for (int i = 0; i < kSnesFeatureCount; ++i) bits |= uint8_t(Has(SnesFeatureAt(i)) ? 1u << i : 0u);
    return bits;
  }
  static SnesGameplaySet FromBits(uint8_t bits) {
    SnesGameplaySet set;
    for (int i = 0; i < kSnesFeatureCount; ++i) set.Set(SnesFeatureAt(i), (bits & (1u << i)) != 0);
    return set;
  }
  // The left side keeps its Dash mode where the right side supports Dash at all.
  SnesGameplaySet operator&(const SnesGameplaySet& other) const {
    SnesGameplaySet out = FromBits(uint8_t(Bits() & other.Bits()));
    if (out.Has(SnesFeature::kDash)) out.dash = dash;
    return out;
  }
  friend bool operator==(const SnesGameplaySet& a, const SnesGameplaySet& b) {
    return a.Bits() == b.Bits() && a.dash == b.dash;
  }
};

// What a mechanic depends on to behave the same on every machine; 0 when it is unsupported.
using SnesCompat = std::array<uint32_t, kSnesFeatureCount>;
// Other players' compatibility ids that do not agree with each other.
inline constexpr uint32_t kSnesCompatMixed = 0xFFFFFFFFu;

// What one client announces: the mechanics it can run with its data, and what its player chose for their own turtle.
struct SnesGameplayCapabilities {
  SnesGameplaySet supports;
  SnesGameplaySet enabled;
  SnesCompat compat{};

  // A client cannot enable or identify what it does not support.
  SnesGameplayCapabilities Normalized() const {
    SnesGameplayCapabilities out{supports, {}, {}};
    out.enabled = enabled & supports;
    for (int i = 0; i < kSnesFeatureCount; ++i) {
      const SnesFeature feature = SnesFeatureAt(i);
      out.compat[size_t(i)] = supports.Has(feature) ? compat[size_t(i)] : 0;
    }
    return out;
  }
  friend bool operator==(const SnesGameplayCapabilities& a, const SnesGameplayCapabilities& b) {
    return a.supports == b.supports && a.enabled == b.enabled && a.compat == b.compat;
  }
};

constexpr bool SnesCompatible(uint32_t a, uint32_t b) { return a == b && a != kSnesCompatMixed; }

// A mechanic is available in a match only when every machine supports it with the same data. Who turned it on does
// not matter here: each player's choice applies to their own turtle.
SnesGameplaySet SupportedSnes(const SnesGameplayCapabilities& a, const SnesGameplayCapabilities& b);

enum class SnesStatus {
  kAvailable,
  kUnsupportedLocally,
  kUnsupportedByOther,
  kUnsupportedByService,
  kWaiting,
  kIncompatible,
};

// `peer` is empty while the other players' capabilities are not known yet.
SnesStatus SnesStatusOf(SnesFeature feature, const SnesGameplayCapabilities& local,
                        const std::optional<SnesGameplayCapabilities>& peer, bool service_supported = true);

// The mechanics available to the match and each seat's own choice among them.
struct SnesMatchTable {
  SnesGameplaySet supported;
  std::array<std::optional<SnesGameplaySet>, kSnesSeats> players{};

  // The mechanics the turtle of `slot` uses; nothing for an unknown seat.
  SnesGameplaySet For(int slot) const {
    if (slot < 1 || slot > kSnesSeats || !players[size_t(slot - 1)]) return {};
    return *players[size_t(slot - 1)] & supported;
  }
  bool AnyOn() const {
    for (int slot = 1; slot <= kSnesSeats; ++slot) {
      if (For(slot).Any()) return true;
    }
    return false;
  }
  friend bool operator==(const SnesMatchTable& a, const SnesMatchTable& b) {
    for (int slot = 1; slot <= kSnesSeats; ++slot) {
      if (!(a.For(slot) == b.For(slot))) return false;
    }
    return a.supported == b.supported;
  }
};

// The table the room fixed for one gameplay match of the party, numbered from 1 by game start.
struct SnesMatchLock {
  int round = 0;
  SnesMatchTable table;
};

// What a client knows of its room: `peer` is the other players combined (supported only when every one of them does,
// the id only when they agree) and empty until all of them have announced, `table` what the service makes of everyone,
// and `locked` the latest table it fixed for a match.
struct MatchSnes {
  std::optional<SnesGameplayCapabilities> peer;
  SnesMatchTable table;
  std::optional<SnesMatchLock> locked;
};

// The service's supported set checked against the client's own rule; it can only be smaller.
SnesGameplaySet VerifiedSupported(const SnesGameplayCapabilities& local, const MatchSnes& room);

// The service's locked table, used whole or not at all: every player runs the same check over the same announcements,
// so a table any of them cannot back becomes all Off for every one of them.
SnesMatchTable VerifiedLock(const SnesGameplayCapabilities& local, const MatchSnes& room);

}  // namespace tmnt::online
