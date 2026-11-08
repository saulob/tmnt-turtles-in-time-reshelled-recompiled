#pragma once

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace tmnt {

inline constexpr char kClassicSoundtrackKey[] = "F9";

// Adds the transient "SNES soundtrack" message under the frame counter and the F9 switch between the SNES and the
// original music. The message shows for a few seconds when the SNES music starts and after every F9.
void CreateClassicSoundtrackOverlay(rex::ui::ImGuiDrawer* drawer);

// Whether the message is on screen; the Ping panel moves down while it is.
bool ClassicSoundtrackNoticeVisible();

}  // namespace tmnt
