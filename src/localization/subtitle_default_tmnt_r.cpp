// Localization_GetDefaultSubtitleSettings: English defaults to no subtitles because its speech needs none. pt-BR runs on
// the English slot, but its players read the subtitles.
#include <rex/cvar.h>
#include <rex/hook.h>

#include "language.h"

REX_EXTERN(__imp__sub_875A2C70);
REX_HOOK_RAW(sub_875A2C70) {
  __imp__sub_875A2C70(ctx, base);
  if (rex::cvar::GetFlagByName(tmnt::kLanguageSetting) == tmnt::kPortugueseBrazilKey) ctx.r3.u64 = 1;
}
