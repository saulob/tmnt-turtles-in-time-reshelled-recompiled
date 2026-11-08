#include "classic_soundtrack/activation.h"

#include <exception>
#include <utility>

#include "classic_soundtrack/music_streams.h"

namespace tmnt::classic {
namespace {

std::string Fallback(const std::string& reason) {
  return "Classic soundtrack: the original music plays because " + reason;
}

}  // namespace

bool ClassicSoundtrackActivation::Start(bool enabled, const std::filesystem::path& user_data_root,
                                        const ProviderRegistration& register_provider,
                                        ClassicSoundtrackProvider::Warn warn, ClassicSoundtrackProvider::Warn note,
                                        const ActivationLog& log) {
  ready_.store(false, std::memory_order_release);
  RuntimeStartResult start = PrepareRuntimeStart(true, user_data_root);
  if (start.start != RuntimeStart::kReady) {
    if (enabled) {
      log(true, start.message);
    } else {
      log(false, "Classic soundtrack: Off");
    }
    return false;
  }
  log(false, start.message);

  try {
    provider_ = std::make_unique<ClassicSoundtrackProvider>(std::move(start.set), warn, kMusicStreams, note);
  } catch (const std::exception& error) {
    provider_.reset();
    log(true, Fallback(std::string("the SNES music player could not be created: ") + error.what()));
    return false;
  }
  provider_->SetReplacing(enabled);
  callbacks_ = provider_->Callbacks();
  if (!register_provider || !register_provider(&callbacks_)) {
    provider_.reset();
    callbacks_ = {};
    log(true, Fallback("the audio runtime did not accept the music provider"));
    return false;
  }
  ready_.store(true, std::memory_order_release);
  if (enabled) {
    log(false, "Classic soundtrack: enabled, " + std::to_string(kMusicStreams.size()) + " music streams");
  } else {
    log(false, "Classic soundtrack: Off");
  }
  return active();
}

bool ClassicSoundtrackActivation::SetReplacing(bool on) {
  if (!ready() || !provider_) {
    return false;
  }
  provider_->SetReplacing(on);
  return true;
}

}  // namespace tmnt::classic
