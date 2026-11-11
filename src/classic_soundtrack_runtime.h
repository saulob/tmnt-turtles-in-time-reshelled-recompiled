#pragma once

#include <filesystem>

#include "classic_soundtrack/activation.h"

namespace tmnt {

inline constexpr char kClassicSoundtrackSetting[] = "tmnt_classic_soundtrack";

// With a Ready import, hands the runtime the SNES music provider, which replaces the music when the
// tmnt_classic_soundtrack setting is on. Any other state leaves the original music untouched. Runs once, before the
// guest starts.
void StartClassicSoundtrack(const std::filesystem::path& user_data_root);

// With a registered provider, switches between the SNES and the original music for this session only. False without one.
bool SetClassicSoundtrackActive(bool on);

// The provider is registered, so the SNES music can be switched on and off.
bool IsClassicSoundtrackReady();

// True only while the SNES music is what the player hears.
bool IsClassicSoundtrackActive();

}  // namespace tmnt
