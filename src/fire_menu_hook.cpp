// The archive's decompressor: the menu movies and, under pt-BR, the font are patched as their bundle is unpacked.
#include <rex/hook.h>
#include <rex/logging.h>

#include "fire_movie_patch.h"
#include "guest_memory.h"
#include "localization/active_language.h"
#include "localization/font_patch.h"

// (compressed, compressed size, output, output size pointer)
REX_EXTERN(__imp__sub_824EC4C8);
REX_HOOK_RAW(sub_824EC4C8) {
  const uint32_t output = ctx.r5.u32;
  const uint32_t size_pointer = ctx.r6.u32;
  __imp__sub_824EC4C8(ctx, base);
  if (!output || !size_pointer) return;
  const uint32_t size = tmnt::LoadBE32(tmnt::GuestHost(base, size_pointer), 0);
  if (tmnt::IsMenuBundle(size)) {
    const tmnt::BundlePatchResult result = tmnt::PatchMenuBundle(tmnt::GuestHost(base, output), size);
    if (result.failed) {
      REXLOG_WARN("Menu movies: {} patched, {} left unchanged in a bundle of {} bytes", result.patched, result.failed, size);
    } else {
      REXLOG_INFO("Menu movies: {} patched in a bundle of {} bytes", result.patched, size);
    }
  }
  // After the movie edits: they check each movie's hash, and the font is part of it.
  if (tmnt::localization::PortugueseBrazilSelected() && tmnt::localization::IsFontBundle(size)) {
    const auto font = tmnt::localization::AddPortugueseGlyphs(tmnt::GuestHost(base, output), size);
    REXLOG_INFO("Language: pt-BR font descriptors={} movie_fonts={} atlases={} in a bundle of {} bytes", font.descriptors,
                font.movie_fonts, font.atlases, size);
  }
}
