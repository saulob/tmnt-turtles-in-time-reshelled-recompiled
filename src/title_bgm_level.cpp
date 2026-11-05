#include "title_bgm_level.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game_files.h"

REXCVAR_DEFINE_INT32(tmnt_title_music_level, tmnt::kDefaultMusicLevel, "TMNT",
                     "Music level of the saved profile, set by the port at start for the title music");

namespace tmnt {

void PublishTitleMusicLevel(const std::filesystem::path& user_data_root) {
  const int32_t level = ReadSavedMusicLevel(SavedSettingsPath(user_data_root, kTitleId));
  rex::cvar::SetFlagByName("tmnt_title_music_level", std::to_string(level));
  REXLOG_INFO("Saved music level: {}", level);
}

}  // namespace tmnt
