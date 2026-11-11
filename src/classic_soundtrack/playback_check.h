#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "classic_soundtrack/soundset.h"
#include "classic_soundtrack/spc_player.h"

namespace tmnt::classic {

enum class LoopKind { kLooping, kOneShot, kUnknown };

const char* LoopKindText(LoopKind kind);

struct PlaybackCheck {
  size_t song_id = 0;
  size_t pack_id = 0;
  SnesSoundtrackPlayer::StartResult start = SnesSoundtrackPlayer::StartResult::kOk;
  bool emulator_ok = true;
  const char* emulator_error = nullptr;
  std::uint64_t frames = 0;
  bool left_audible = false;
  bool right_audible = false;
  int peak = 0;
  std::uint64_t clipped = 0;
  LoopKind loop = LoopKind::kUnknown;
  double loop_seconds = 0;
  double loop_found_at = 0;
  double audible_until = 0;
  std::vector<int> sources;       // sample directory entries the driver keyed
  std::vector<int> bad_sources;   // keyed entries whose BRR chain is not valid
  bool uses_pack_samples = false; // keyed entries 0-2, which the pack replaces
  bool passed() const;
};

struct VoiceSnapshot {
  int source = 0;
  int pitch = 0;
  int volume_left = 0;
  int volume_right = 0;
  int adsr1 = 0;
  int adsr2 = 0;
  int gain = 0;
  int envelope = 0;
  bool pitch_modulation = false;
  bool noise = false;
};

struct DspSnapshot {
  int main_left = 0;
  int main_right = 0;
  int echo_left = 0;
  int echo_right = 0;
  int echo_feedback = 0;
  int flags = 0;
  int directory = 0;
  int echo_start = 0;
  int echo_delay = 0;
  int echo_enable = 0;
  std::array<int, 8> fir{};
  std::array<VoiceSnapshot, 8> voices{};
};

struct SampleInfo {
  int source = 0;
  size_t start = 0;
  size_t loop = 0;
  size_t blocks = 0;       // to the block with the end flag
  size_t loop_blocks = 0;
  size_t range_over_12 = 0;
  bool ended = false;
  bool loops = false;
};

// Developer inspection of the emulated DSP and of the BRR chain a directory entry points at.
DspSnapshot ReadDsp(SnesSoundtrackPlayer& player);
SampleInfo DescribeSample(SnesSoundtrackPlayer& player, int source);

// Developer validation: runs one song on the emulated driver for up to max_seconds and watches the DSP voices and
// the driver's sequence pointers. Never part of the game.
PlaybackCheck CheckSong(const SoundSet& set, size_t song_id, size_t pack_id, double max_seconds);

}  // namespace tmnt::classic
