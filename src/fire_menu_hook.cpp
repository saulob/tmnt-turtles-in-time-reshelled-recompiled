// The archive's decompressor: the menu movies are patched as their world bundle is unpacked.
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
  if (!tmnt::IsMenuBundle(size)) return;
  const tmnt::BundlePatchResult result = tmnt::PatchMenuBundle(tmnt::GuestHost(base, output), size);
  if (result.failed) {
    REXLOG_WARN("Menu movies: {} patched, {} left unchanged in a bundle of {} bytes", result.patched, result.failed, size);
  } else {
    REXLOG_INFO("Menu movies: {} patched in a bundle of {} bytes", result.patched, size);
  }
}
