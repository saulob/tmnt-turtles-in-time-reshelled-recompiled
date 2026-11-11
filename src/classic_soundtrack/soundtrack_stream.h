#pragma once

#include <cstddef>
#include <cstdint>

#include "classic_soundtrack/resampler.h"
#include "classic_soundtrack/spc_player.h"

namespace tmnt::classic {

// One SNES song at the rate the host asks for.
class SoundtrackStream {
 public:
  SnesSoundtrackPlayer::StartResult Start(const SoundSet& set, size_t song_id, size_t pack_id,
                                          std::uint32_t output_rate);
  void Stop();

  // Interleaved stereo at the output rate; false once the emulator stops or faults.
  bool Render(std::int16_t* frames, size_t frame_count);

  bool playing() const { return player_.playing(); }
  SnesSoundtrackPlayer& player() { return player_; }
  const StereoResampler& resampler() const { return resampler_; }

 private:
  SnesSoundtrackPlayer player_;
  StereoResampler resampler_;
};

}  // namespace tmnt::classic
