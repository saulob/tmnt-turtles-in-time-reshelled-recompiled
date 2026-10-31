#pragma once

#include <functional>

#include "gamer_card_rules.h"

namespace rex::system {
class AchievementManager;
class KernelState;
}  // namespace rex::system

namespace tmnt {

struct GamerCardBridge {
  std::function<void(GamerCardRequest)> open;
};

// Installs the XamShowGamerCardUIForXUID bridge. Called before the game starts.
void InstallGamerCard(GamerCardBridge bridge);

// What the local profile, its achievements and the local stats hold. Call it on the thread that owns the overlay.
GamerCardSources ReadGamerCardSources(rex::system::KernelState& kernel,
                                      const rex::system::AchievementManager& achievements);

}  // namespace tmnt
