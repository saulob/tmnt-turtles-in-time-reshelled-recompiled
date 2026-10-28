#pragma once

#include <cstdint>
#include <vector>

#include "guest_memory.h"

namespace rex::system {
class KernelState;
}  // namespace rex::system

namespace tmnt {

// ReXGlue's achievement store refuses ID 0, which some titles use for their first achievement. Achievement 0 is
// kept under this ID in the store and swapped in only while the game's write is delivered.
constexpr uint32_t kZeroAchievementAlias = 0x10000;

constexpr uint32_t kAchievementEntrySize = 8;
constexpr uint32_t kAchievementIdOffset = 4;
constexpr uint32_t kMaxAchievementWrites = 64;

// Registers achievement 0 under the alias with the metadata the title's XDBF holds for it.
bool RegisterZeroAchievementAlias(rex::system::KernelState& kernel);

// Where the achievement should appear in the list: by the game's own number.
constexpr uint32_t GameAchievementNumber(uint32_t store_id) {
  return store_id == kZeroAchievementAlias ? 0 : store_id;
}

// XUSER_ACHIEVEMENT[count] {user index, achievement id}: points entries that name achievement 0 at the alias.
// Returns the addresses of the rewritten ids for RestoreZeroAchievementIds.
inline std::vector<uint32_t> AliasZeroAchievementIds(uint8_t* base, uint32_t entries, uint32_t count) {
  std::vector<uint32_t> rewritten;
  if (!entries || count > kMaxAchievementWrites) return rewritten;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id_at = entries + i * kAchievementEntrySize + kAchievementIdOffset;
    if (LoadBE32(base, id_at) != 0) continue;
    StoreBE32(base, id_at, kZeroAchievementAlias);
    rewritten.push_back(id_at);
  }
  return rewritten;
}

inline void RestoreZeroAchievementIds(uint8_t* base, const std::vector<uint32_t>& rewritten) {
  for (const uint32_t id_at : rewritten) StoreBE32(base, id_at, 0);
}

}  // namespace tmnt
