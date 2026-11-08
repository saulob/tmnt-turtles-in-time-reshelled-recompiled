#pragma once

#include <filesystem>

#include "online/snes_features.h"
#include "snes_menu_rules.h"

namespace tmnt {

// Publishes the values derived from the imported SNES ROM for TMNT_r before the guest starts. Without a Ready import
// the game stays stock.
void StartSnesGameplay(const std::filesystem::path& user_data_root);

// What the F9 menu can offer this session: the soundtrack provider and the gameplay data are checked apart.
snes::Availability SnesAvailability();

// The F9 menu's value of a feature: its session override if there is one, otherwise the launcher's saved value.
bool SnesFeatureEnabled(snes::Feature feature);

// Dash is a mode, not a switch: the F9 menu's value of it, which `SnesFeatureEnabled(kDash)` only reports as on or off.
snes::DashMode SnesDashMode();

// The gameplay mechanics this player supports and has turned on, for the online negotiation.
online::SnesGameplayCapabilities LocalSnesCapabilities();

// Once per frame: publishes the gameplay mechanics locked for the online match to the game module.
void UpdateSnesOnline();

// What the F9 menu shows of the online negotiation.
snes::OnlineView SnesOnline();

// Switches a feature for this session only. The launcher's saved value and settings.toml are never touched. Classic
// Soundtrack switches at once, online too; a gameplay mechanic changed during a locked online match waits for the next.
void ToggleSnesFeatureForSession(snes::Feature feature);

// Moves the Dash mode one step through Off, Auto, Manual (-1 goes back) for this session only.
void StepSnesDashForSession(int step);

}  // namespace tmnt
