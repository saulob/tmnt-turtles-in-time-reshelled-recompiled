#pragma once

#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace tmnt {

// The in-game friend list behind the lobby's Invite Friends, and the prompt for a friend's invite. `post` runs work on
// the UI thread.
void CreateInviteUi(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post);

// Opens the friend list. False when the in-game UI is not up, so the game keeps its own behavior.
bool OpenInvitePicker();

}  // namespace tmnt
