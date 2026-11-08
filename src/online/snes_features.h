#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tmnt::online {

// The SNES gameplay mechanics an online match negotiates. Classic Soundtrack is local presentation and is never here.
enum class SnesFeature : uint8_t { kDirectionalCombat = 0, kThrowControl = 1, kDash = 2 };
inline constexpr int kSnesFeatureCount = 3;
// Party slots 1 to 4, the room's seats.
inline constexpr int kSnesSeats = 4;

constexpr SnesFeature SnesFeatureAt(int index) { return SnesFeature(index); }

// The wire key. Header only: the protocol parser uses it in targets that do not link the negotiation rule.
constexpr const char* SnesKey(SnesFeature feature) {
  switch (feature) {
    case SnesFeature::kDirectionalCombat: return "directional_combat";
    case SnesFeature::kThrowControl: return "throw_control";
    case SnesFeature::kDash: return "dash";
  }
  return "";
}

const char* SnesLabel(SnesFeature feature);

struct SnesGameplaySet {
  bool directional_combat = false;
  bool throw_control = false;
  bool dash = false;

  bool Has(SnesFeature feature) const {
    switch (feature) {
      case SnesFeature::kDirectionalCombat: return directional_combat;
      case SnesFeature::kThrowControl: return throw_control;
      case SnesFeature::kDash: return dash;
    }
    return false;
  }
  void Set(SnesFeature feature, bool value) {
    switch (feature) {
      case SnesFeature::kDirectionalCombat: directional_combat = value; break;
      case SnesFeature::kThrowControl: throw_control = value; break;
      case SnesFeature::kDash: dash = value; break;
    }
  }
  bool Any() const { return Bits() != 0; }
  uint8_t Bits() const {
    return uint8_t((directional_combat ? 1u : 0u) | (throw_control ? 2u : 0u) | (dash ? 4u : 0u));
  }
  static SnesGameplaySet FromBits(uint8_t bits) { return {(bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0}; }
  SnesGameplaySet operator&(const SnesGameplaySet& other) const { return FromBits(uint8_t(Bits() & other.Bits())); }
  friend bool operator==(const SnesGameplaySet& a, const SnesGameplaySet& b) { return a.Bits() == b.Bits(); }
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
    for (int i = 0; i < kSnesFeatureCount; ++i) {
      const SnesFeature feature = SnesFeatureAt(i);
      out.enabled.Set(feature, enabled.Has(feature) && supports.Has(feature));
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
