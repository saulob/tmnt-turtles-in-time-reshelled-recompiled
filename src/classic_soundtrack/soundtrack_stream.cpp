#include "classic_soundtrack/soundtrack_stream.h"

#include "classic_soundtrack/loudness.h"

namespace tmnt::classic {

SnesSoundtrackPlayer::StartResult SoundtrackStream::Start(const SoundSet& set, size_t song_id, size_t pack_id,
                                                          std::uint32_t output_rate) {
  if (!resampler_.Configure(SnesSoundtrackPlayer::kSampleRate, output_rate)) {
    player_.Stop();
    return SnesSoundtrackPlayer::StartResult::kEmulatorError;
  }
  return player_.Start(set, song_id, pack_id);
}

void SoundtrackStream::Stop() {
  player_.Stop();
  resampler_.Reset();
}

bool SoundtrackStream::Render(std::int16_t* frames, size_t frame_count) {
  if (!player_.playing()) {
    return false;
  }
  const bool complete = resampler_.Process(
      frames, frame_count, [this](std::int16_t* dst, size_t count) { return player_.Render(dst, count); });
  LiftSnesSoundtrack(frames, frame_count * 2);
  return complete && player_.playing();
}

}  // namespace tmnt::classic
