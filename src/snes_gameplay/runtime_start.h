#pragma once

#include <string>

#include "snes_gameplay/gameplay_data.h"

namespace tmnt::snes {

struct RuntimeStart {
  std::string data;  // the runtime cvar value, empty when SNES Gameplay cannot run
  std::string message;
  bool warning = false;
};

// What the game does at start: the derived values with a Ready import, otherwise the stock game and one log line.
RuntimeStart PrepareRuntimeStart(const GameplayStatusResult& status);

}  // namespace tmnt::snes
