#include <atomic>
#include <cstdint>
#include <optional>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "title_bgm_level.h"

using namespace tmnt;

namespace {

constexpr uint32_t kUniverseImport = 5;
constexpr uint32_t kUniverseBgmVolumeOffset = 28;

}  // namespace

REX_EXTERN(__imp__sub_87520018);
REX_EXTERN(__imp__sub_87607E18);
REX_EXTERN(__imp__sub_87608560);

// The title's SoundBGM_Init runs before the profile loads, so it would play at the default level until Start applies the
// saved one. Seeded with the saved level, the load at Start changes nothing.
REX_HOOK_RAW(sub_87608560) {
  static std::atomic<bool> seeded{false};
  if (!seeded.exchange(true)) {
    const std::optional<int32_t> level = TitleMusicSeed(rex::cvar::GetFlagByName("tmnt_title_music_level"));
    const uint32_t self = ctx.r3.u32;
    const uint64_t lr = ctx.lr;
    ctx.r3.u64 = kUniverseImport;
    __imp__sub_87520018(ctx, base);
    PPCFunc* get_universe = rex::runtime::ResolveIndirectFunction(ctx.r3.u32);
    if (level && get_universe) {
      ctx.lr = lr;
      get_universe(ctx, base);
      StoreBE32(base, ctx.r3.u32 + kUniverseBgmVolumeOffset, uint32_t(*level));
      REXLOG_INFO("DEBUG_SAULO title music seed level={}", *level);
    }
    ctx.r3.u64 = self;
    ctx.lr = lr;
  }
  __imp__sub_87608560(ctx, base);
}

// DEBUG_SAULO: SoundBGM_ChangeVolumeWithoutSaving(this, level), every level the music actually takes.
REX_HOOK_RAW(sub_87607E18) {
  REXLOG_INFO("DEBUG_SAULO bgm level={}", ctx.r4.s32);
  __imp__sub_87607E18(ctx, base);
}
