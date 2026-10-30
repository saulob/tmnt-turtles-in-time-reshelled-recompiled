#include <atomic>

#include <rex/hook.h>

#include "guest_memory.h"

using namespace tmnt;

namespace {

constexpr uint32_t kUniverseImport = 5;
constexpr uint32_t kUniverseBgmVolumeOffset = 28;
constexpr uint32_t kTitleMusicLevel = 6;

}  // namespace

REX_EXTERN(__imp__sub_87520018);
REX_EXTERN(__imp__sub_87608560);

// The title's SoundBGM_Init runs before the profile loads and would use the default music level 10.
REX_HOOK_RAW(sub_87608560) {
  static std::atomic<bool> seeded{false};
  if (!seeded.exchange(true)) {
    const uint32_t self = ctx.r3.u32;
    const uint64_t lr = ctx.lr;
    ctx.r3.u64 = kUniverseImport;
    __imp__sub_87520018(ctx, base);
    if (PPCFunc* get_universe = rex::runtime::ResolveIndirectFunction(ctx.r3.u32)) {
      ctx.lr = lr;
      get_universe(ctx, base);
      StoreBE32(base, ctx.r3.u32 + kUniverseBgmVolumeOffset, kTitleMusicLevel);
    }
    ctx.r3.u64 = self;
    ctx.lr = lr;
  }
  __imp__sub_87608560(ctx, base);
}
