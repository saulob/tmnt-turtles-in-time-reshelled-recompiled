#pragma once

#include <filesystem>
#include <string>

#include "classic_soundtrack/soundset.h"

namespace tmnt::classic {

enum class RuntimeStart { kOff, kReady, kFallback };

struct RuntimeStartResult {
  RuntimeStart start = RuntimeStart::kOff;
  std::string message;  // one log line
  std::string region;
  SoundSet set;         // filled when kReady
};

// What the game does at start: nothing when the setting is off, the validated sound set when the import is Ready,
// otherwise the original music. Reads only <user_data_root>/snes_soundtrack.
RuntimeStartResult PrepareRuntimeStart(bool enabled, const std::filesystem::path& user_data_root);

}  // namespace tmnt::classic
