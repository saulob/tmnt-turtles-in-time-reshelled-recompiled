// The archive's decompressor: the Online menu movie is patched once it is unpacked.
#include <rex/hook.h>
#include <rex/logging.h>

#include "fire_movie_patch.h"
#include "guest_memory.h"

// (compressed, compressed size, output, output size pointer)
REX_EXTERN(__imp__sub_824EC4C8);
REX_HOOK_RAW(sub_824EC4C8) {
  const uint32_t output = ctx.r5.u32;
  const uint32_t size_pointer = ctx.r6.u32;
  __imp__sub_824EC4C8(ctx, base);
  if (!output || !size_pointer) return;
  const uint32_t size = tmnt::LoadBE32(tmnt::GuestHost(base, size_pointer), 0);
  if (size != tmnt::kOnlineMenuMovieBytes) return;
  if (tmnt::PatchOnlineMenuMovie(tmnt::GuestHost(base, output), size)) {
    REXLOG_INFO("Online menu movie: Quick, Create and Custom Match only");
  }
}
