#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "classic_soundtrack/runtime_start.h"
#include "classic_soundtrack/xma_provider.h"

namespace tmnt::classic {

// Receives each log line; warning is true for the fallback line.
using ActivationLog = std::function<void(bool warning, const std::string& line)>;
// Hands the provider callbacks to the audio runtime; false when it refuses them.
using ProviderRegistration = std::function<bool(const RexXmaPcmProvider* callbacks)>;

// Owns everything the audio runtime keeps pointers to, so it must live as long as the process.
class ClassicSoundtrackActivation {
 public:
  // With a Ready import the sound set is loaded and the provider registered whatever the setting says, so the music
  // can be switched at run time; the setting only decides whether it starts replacing. Any other outcome leaves the
  // original music in place and logs the reason (a warning only when the setting asked for the SNES music).
  bool Start(bool enabled, const std::filesystem::path& user_data_root, const ProviderRegistration& register_provider,
             ClassicSoundtrackProvider::Warn warn, ClassicSoundtrackProvider::Warn note, const ActivationLog& log);

  // Runtime override of the starting choice; the setting itself is not touched. False when no provider is registered.
  bool SetReplacing(bool on);

  // The provider is registered with the runtime and can replace music.
  bool ready() const { return ready_.load(std::memory_order_acquire); }
  // The SNES music is what the player hears.
  bool active() const { return ready() && provider_ && provider_->replacing(); }
  const ClassicSoundtrackProvider* provider() const { return provider_.get(); }

 private:
  std::unique_ptr<ClassicSoundtrackProvider> provider_;
  RexXmaPcmProvider callbacks_{};
  std::atomic<bool> ready_{false};
};

}  // namespace tmnt::classic
