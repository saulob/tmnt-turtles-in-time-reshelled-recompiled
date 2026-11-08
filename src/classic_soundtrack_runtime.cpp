#include "classic_soundtrack_runtime.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "classic_soundtrack/activation.h"
#include "settings.h"

// Exported by the patched runtime (patch 0087).
extern "C" void rex_audio_set_xma_pcm_provider(const RexXmaPcmProvider* provider);

namespace tmnt {
namespace {

void WarnFromAudio(const char* message) {
  REXLOG_WARN("{}", message);
}

void NoteFromAudio(const char* message) {
  REXLOG_INFO("{}", message);
}

// Never destroyed: the audio threads keep calling the provider until the process exits.
classic::ClassicSoundtrackActivation& Activation() {
  static auto* activation = new classic::ClassicSoundtrackActivation;
  return *activation;
}

}  // namespace

void StartClassicSoundtrack(const std::filesystem::path& user_data_root) {
  Activation().Start(
      REXCVAR_GET(tmnt_classic_soundtrack), user_data_root,
      [](const RexXmaPcmProvider* callbacks) {
        rex_audio_set_xma_pcm_provider(callbacks);
        return true;
      },
      WarnFromAudio, NoteFromAudio, [](bool warning, const std::string& line) {
        if (warning) {
          REXLOG_WARN("{}", line);
        } else {
          REXLOG_INFO("{}", line);
        }
      });
}

classic::RuntimeToggle ToggleClassicSoundtrack() {
  return Activation().Toggle();
}

bool IsClassicSoundtrackActive() {
  return Activation().active();
}

}  // namespace tmnt
