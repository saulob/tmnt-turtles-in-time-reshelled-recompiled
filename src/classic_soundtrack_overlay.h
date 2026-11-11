#pragma once

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace tmnt {

// Adds the transient "SNES soundtrack" message under the frame counter. It shows for a few seconds when the SNES music
// starts with the game.
void CreateClassicSoundtrackOverlay(rex::ui::ImGuiDrawer* drawer);

// Whether the message is on screen; the Ping panel moves down while it is.
bool ClassicSoundtrackNoticeVisible();

// The same corner message, for F10 pressed while Online play is enabled.
void ShowCheatsUnavailableNotice();

}  // namespace tmnt
