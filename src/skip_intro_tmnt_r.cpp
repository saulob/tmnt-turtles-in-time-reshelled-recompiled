#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "skip_intro.h"

using namespace tmnt;

namespace {

constexpr uint32_t kUniverseVars = 8;
constexpr uint32_t kNextLoadingScreen = 7208;
constexpr uint32_t kNextWorldToLoad = 7212;

}  // namespace

REX_EXTERN(__imp__sub_87602DA8);

// Universe_NextWorldNow. Before the original, so the loading world it starts reads the title as its target.
REX_HOOK_RAW(sub_87602DA8) {
  const uint32_t universe = LoadBE32(base, ctx.r3.u32 + kUniverseVars);
  const uint32_t next = LoadBE32(base, universe + kNextWorldToLoad);
  const uint32_t target = StartupWorldTarget(next, SkipIntroEnabled(rex::cvar::GetFlagByName("tmnt_skip_intro")));
  REXLOG_INFO("DEBUG_SAULO world transition loading={:08X} next={:08X} target={:08X}",
              LoadBE32(base, universe + kNextLoadingScreen), next, target);
  if (target != next) {
    StoreBE32(base, universe + kNextWorldToLoad, target);
    REXLOG_INFO("Skip intro: startup goes to the title screen");
  }
  __imp__sub_87602DA8(ctx, base);
}
