#pragma once

#include <functional>

namespace tmnt {

struct AchievementMenuBridge {
  std::function<void()> open;
  std::function<void()> close;
};

// Installs the XamShowAchievementsUI bridge and the guest controller gate. Called before the game starts.
void InstallAchievementMenu(AchievementMenuBridge bridge);

// The achievements list or the Gamer Card is up, or the B that closed it is still held.
void SetAchievementMenuOpen(bool open);
bool AchievementMenuHoldsInput();

// Scroll direction read from the game's controller while the overlay is up: -1 up .. 1 down.
float AchievementMenuScrollAxis();

}  // namespace tmnt
