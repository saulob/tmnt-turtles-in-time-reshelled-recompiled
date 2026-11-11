#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "classic_soundtrack/soundset.h"

struct Snes_Spc;

namespace tmnt::classic {

// Runs the game's own SPC700 sound driver from a sound set. All memory is allocated by the constructor.
class SnesSoundtrackPlayer {
 public:
  static constexpr std::uint32_t kSampleRate = 32000;

  enum class StartResult { kOk, kBadSong, kBadPack, kEmulatorError, kNoAcknowledge };

  SnesSoundtrackPlayer();
  ~SnesSoundtrackPlayer();
  SnesSoundtrackPlayer(const SnesSoundtrackPlayer&) = delete;
  SnesSoundtrackPlayer& operator=(const SnesSoundtrackPlayer&) = delete;

  // Boots the driver on a fresh machine holding the boot image, the pack and the song, then sends the song command.
  StartResult Start(const SoundSet& set, size_t song_id, size_t pack_id);
  void Stop();

  // Interleaved stereo at kSampleRate. Returns frames written; 0 when stopped or after an emulator fault.
  size_t Render(std::int16_t* frames, size_t frame_count);

  bool playing() const { return playing_; }
  const char* error() const { return error_; }
  Snes_Spc& emulator() { return *spc_; }

 private:
  bool RunFrames(size_t frame_count);
  bool SendCommand(std::uint8_t command);

  std::unique_ptr<Snes_Spc> spc_;
  std::vector<unsigned char> image_;
  std::array<std::int16_t, 256> scratch_{};
  bool playing_ = false;
  const char* error_ = nullptr;
};

const char* StartResultText(SnesSoundtrackPlayer::StartResult result);

}  // namespace tmnt::classic
