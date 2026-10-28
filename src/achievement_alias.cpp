#include "achievement_alias.h"

#include <utility>

#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

namespace tmnt {

bool RegisterZeroAchievementAlias(rex::system::KernelState& kernel) {
  const auto db = kernel.title_xdbf();
  if (!db.is_valid()) return false;
  const auto language = db.GetExistingLanguage(static_cast<rex::system::XLanguage>(REXCVAR_GET(user_language)));
  for (const auto& entry : db.GetAchievements()) {
    if (entry.id != 0) continue;
    rex::system::AchievementInfo info;
    info.id = kZeroAchievementAlias;
    info.label = db.GetStringTableEntry(language, entry.label_id);
    info.description = db.GetStringTableEntry(language, entry.description_id);
    info.unachieved_description = db.GetStringTableEntry(language, entry.unachieved_id);
    info.image_id = entry.image_id;
    info.gamerscore = entry.gamerscore;
    info.flags = entry.flags;
    return kernel.achievements().RegisterAchievement(std::move(info));
  }
  return false;
}

}  // namespace tmnt
