#include "online/snes_features.h"

namespace tmnt::online {

const char* SnesLabel(SnesFeature feature) {
  switch (feature) {
    case SnesFeature::kDirectionalCombat: return "Directional Combat";
    case SnesFeature::kThrowControl: return "SNES Throw Control";
    case SnesFeature::kDash: return "SNES Dash";
    case SnesFeature::kDoubleJump: return "Disable Double Jump";
  }
  return "";
}

SnesGameplaySet SupportedSnes(const SnesGameplayCapabilities& a, const SnesGameplayCapabilities& b) {
  const SnesGameplayCapabilities left = a.Normalized();
  const SnesGameplayCapabilities right = b.Normalized();
  SnesGameplaySet out;
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const SnesFeature feature = SnesFeatureAt(i);
    out.Set(feature, left.supports.Has(feature) && right.supports.Has(feature) &&
                         SnesCompatible(left.compat[size_t(i)], right.compat[size_t(i)]));
  }
  return out;
}

SnesStatus SnesStatusOf(SnesFeature feature, const SnesGameplayCapabilities& local,
                        const std::optional<SnesGameplayCapabilities>& peer, bool service_supported) {
  const SnesGameplayCapabilities mine = local.Normalized();
  if (!mine.supports.Has(feature)) return SnesStatus::kUnsupportedLocally;
  if (!service_supported) return SnesStatus::kUnsupportedByService;
  if (!peer) return SnesStatus::kWaiting;
  const SnesGameplayCapabilities theirs = peer->Normalized();
  if (!theirs.supports.Has(feature)) return SnesStatus::kUnsupportedByOther;
  if (!SnesCompatible(mine.compat[size_t(feature)], theirs.compat[size_t(feature)])) return SnesStatus::kIncompatible;
  return SnesStatus::kAvailable;
}

SnesGameplaySet VerifiedSupported(const SnesGameplayCapabilities& local, const MatchSnes& room) {
  if (!room.peer) return {};
  return room.table.supported & SupportedSnes(local, *room.peer);
}

SnesMatchTable VerifiedLock(const SnesGameplayCapabilities& local, const MatchSnes& room) {
  if (!room.locked || !room.peer) return {};
  const SnesGameplaySet backed = SupportedSnes(local, *room.peer);
  const SnesMatchTable& table = room.locked->table;
  if (!((table.supported & backed) == table.supported)) return {};
  SnesMatchTable out;
  out.supported = table.supported;
  for (int slot = 1; slot <= kSnesSeats; ++slot) {
    if (table.players[size_t(slot - 1)]) out.players[size_t(slot - 1)] = table.For(slot);
  }
  return out;
}

}  // namespace tmnt::online
