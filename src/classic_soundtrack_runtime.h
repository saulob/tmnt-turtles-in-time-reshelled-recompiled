#pragma once

#include <filesystem>

#include "classic_soundtrack/activation.h"

namespace tmnt {

inline constexpr char kClassicSoundtrackSetting[] = "tmnt_classic_soundtrack";

// With a Ready import, hands the runtime the SNES music provider, which replaces the music when the
// tmnt_classic_soundtrack setting is on. Any other state leaves the original music untouched. Runs once, before the
// guest starts.
void StartClassicSoundtrack(const std::filesystem::path& user_data_root);

// F9: switches between the SNES and the original music for this session only.
classic::RuntimeToggle ToggleClassicSoundtrack();

// True only while the SNES music is what the player hears.
bool IsClassicSoundtrackActive();

}  // namespace tmnt
