#include "classic_soundtrack/playback_check.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

#include "Snes_Spc.h"

// Explicit instantiation ignores access checks, so this validator reads the DSP registers without patching the
// vendored emulator.
namespace {
struct DspTag {};
template <auto Member>
struct DspAccess {
  friend auto DspMemberPointer(DspTag) { return Member; }
};
auto DspMemberPointer(DspTag);
template struct DspAccess<&Snes_Spc::dsp>;
}  // namespace

namespace tmnt::classic {
namespace {

constexpr size_t kChunkFrames = 512;
constexpr double kChunkSeconds = double(kChunkFrames) / SnesSoundtrackPlayer::kSampleRate;
constexpr int kAudibleLevel = 8;
constexpr double kSilenceForEnd = 6.0;
constexpr double kMinimumLoop = 2.0;
constexpr double kSettle = 1.0;
constexpr double kMovingWindow = 30.0;
constexpr size_t kMinimumSteps = 4;
constexpr size_t kRepeatPermille = 995;
constexpr size_t kDriverRamBytes = 0x400;
constexpr int kDspDirectory = 0x5D;
constexpr int kVoiceSource = 0x04;
constexpr int kVoiceEnvelope = 0x08;
constexpr int kPackSources = 3;
constexpr size_t kBrrBlock = 9;

using Snapshot = std::array<std::uint8_t, kDriverRamBytes>;

bool BrrChainValid(const std::uint8_t* ram, size_t start) {
  for (size_t at = start; at + kBrrBlock <= kAramSize; at += kBrrBlock) {
    if (ram[at] & 1) {
      return true;
    }
  }
  return false;
}

std::uint16_t WordAt(const Snapshot& snapshot, size_t addr) {
  return std::uint16_t(snapshot[addr] | snapshot[addr + 1] << 8);
}

// Words of driver RAM that stay inside the song data and move: the per-track sequence pointers.
std::vector<std::vector<std::uint16_t>> SequencePointerSeries(const std::vector<Snapshot>& snapshots, size_t first,
                                                              size_t song_bytes) {
  std::vector<std::vector<std::uint16_t>> series;
  for (size_t addr = 0; addr + 1 < kDriverRamBytes; ++addr) {
    std::vector<std::uint16_t> values;
    bool inside = true;
    for (size_t i = first; i < snapshots.size() && inside; ++i) {
      const std::uint16_t word = WordAt(snapshots[i], addr);
      inside = word >= kSongAddress && word < kSongAddress + song_bytes;
      values.push_back(word);
    }
    if (inside && std::set<std::uint16_t>(values.begin(), values.end()).size() >= 8) {
      series.push_back(std::move(values));
    }
  }
  return series;
}

bool StillMoving(const std::vector<std::uint16_t>& values, size_t window) {
  const size_t from = values.size() > window ? values.size() - window : 0;
  return std::set<std::uint16_t>(values.begin() + std::ptrdiff_t(from), values.end()).size() >= kMinimumSteps;
}

// The shortest period after which a track's pointer repeats for two whole periods up to the end of the run.
// Snapshots fall between driver ticks, so a neighbour one snapshot away also matches, and a few snapshots on a
// note boundary may still miss.
size_t ConfirmedPeriod(const std::vector<std::uint16_t>& values, size_t minimum) {
  const size_t n = values.size();
  const auto matches = [&](size_t i, size_t period) {
    for (int shift = -1; shift <= 1; ++shift) {
      const long long other = (long long)i - (long long)period + shift;
      if (other >= 0 && values[size_t(other)] == values[i]) {
        return true;
      }
    }
    return false;
  };
  for (size_t earlier = n - 1; earlier-- > 0;) {
    if (values[earlier] != values[n - 1]) {
      continue;
    }
    const size_t period = n - 1 - earlier;
    if (period < minimum || 3 * period > n) {
      continue;
    }
    size_t matched = 0;
    for (size_t i = n - 2 * period; i < n; ++i) {
      matched += matches(i, period) ? 1 : 0;
    }
    if (matched * 1000 >= 2 * period * kRepeatPermille) {
      return period;
    }
  }
  return 0;
}

}  // namespace

const char* LoopKindText(LoopKind kind) {
  switch (kind) {
    case LoopKind::kLooping: return "Looping";
    case LoopKind::kOneShot: return "OneShot";
    case LoopKind::kUnknown: return "Unknown";
  }
  return "Unknown";
}

bool PlaybackCheck::passed() const {
  return start == SnesSoundtrackPlayer::StartResult::kOk && emulator_ok && frames > 0 && (left_audible || right_audible) &&
         bad_sources.empty();
}

DspSnapshot ReadDsp(SnesSoundtrackPlayer& player) {
  auto& dsp = player.emulator().*DspMemberPointer(DspTag{});
  DspSnapshot snapshot;
  snapshot.main_left = dsp.read(0x0C);
  snapshot.main_right = dsp.read(0x1C);
  snapshot.echo_left = dsp.read(0x2C);
  snapshot.echo_right = dsp.read(0x3C);
  snapshot.echo_feedback = dsp.read(0x0D);
  snapshot.flags = dsp.read(0x6C);
  snapshot.directory = dsp.read(kDspDirectory);
  snapshot.echo_start = dsp.read(0x6D);
  snapshot.echo_delay = dsp.read(0x7D);
  snapshot.echo_enable = dsp.read(0x4D);
  const int pitch_modulation = dsp.read(0x2D);
  const int noise = dsp.read(0x3D);
  for (int i = 0; i < 8; ++i) {
    snapshot.fir[size_t(i)] = dsp.read(i * 16 + 0x0F);
    VoiceSnapshot& voice = snapshot.voices[size_t(i)];
    voice.source = dsp.read(i * 16 + kVoiceSource);
    voice.pitch = dsp.read(i * 16 + 2) | (dsp.read(i * 16 + 3) & 0x3F) << 8;
    voice.volume_left = std::int8_t(dsp.read(i * 16));
    voice.volume_right = std::int8_t(dsp.read(i * 16 + 1));
    voice.adsr1 = dsp.read(i * 16 + 5);
    voice.adsr2 = dsp.read(i * 16 + 6);
    voice.gain = dsp.read(i * 16 + 7);
    voice.envelope = dsp.read(i * 16 + kVoiceEnvelope);
    voice.pitch_modulation = (pitch_modulation >> i & 1) != 0;
    voice.noise = (noise >> i & 1) != 0;
  }
  return snapshot;
}

SampleInfo DescribeSample(SnesSoundtrackPlayer& player, int source) {
  auto& dsp = player.emulator().*DspMemberPointer(DspTag{});
  const std::uint8_t* ram = player.emulator().smp_ram();
  const size_t entry = (size_t(dsp.read(kDspDirectory)) << 8) + size_t(source) * 4;
  SampleInfo info;
  info.source = source;
  if (entry + 3 >= kAramSize) {
    return info;
  }
  info.start = size_t(ram[entry] | ram[entry + 1] << 8);
  info.loop = size_t(ram[entry + 2] | ram[entry + 3] << 8);
  for (size_t at = info.start; at + kBrrBlock <= kAramSize; at += kBrrBlock) {
    ++info.blocks;
    info.range_over_12 += (ram[at] >> 4) > 12 ? 1 : 0;
    if (ram[at] & 1) {
      info.ended = true;
      info.loops = (ram[at] & 2) != 0;
      info.loop_blocks = info.loops && info.loop >= info.start ? (at - info.loop) / kBrrBlock + 1 : 0;
      break;
    }
  }
  return info;
}

PlaybackCheck CheckSong(const SoundSet& set, size_t song_id, size_t pack_id, double max_seconds) {
  PlaybackCheck check;
  check.song_id = song_id;
  check.pack_id = pack_id;
  SnesSoundtrackPlayer player;
  check.start = player.Start(set, song_id, pack_id);
  if (check.start != SnesSoundtrackPlayer::StartResult::kOk) {
    check.emulator_ok = false;
    check.emulator_error = player.error();
    return check;
  }

  Snes_Spc& spc = player.emulator();
  auto& dsp = spc.*DspMemberPointer(DspTag{});
  std::vector<std::int16_t> pcm(kChunkFrames * 2);
  std::vector<Snapshot> snapshots;
  std::map<int, bool> source_valid;
  const size_t chunks = size_t(max_seconds / kChunkSeconds);
  double last_audible = -1;
  bool ended_silent = false;

  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    if (player.Render(pcm.data(), kChunkFrames) != kChunkFrames) {
      check.emulator_ok = false;
      check.emulator_error = player.error();
      break;
    }
    check.frames += kChunkFrames;
    const double now = double(chunk + 1) * kChunkSeconds;
    for (size_t i = 0; i < pcm.size(); ++i) {
      const int sample = pcm[i];
      const int level = sample < 0 ? -sample : sample;
      check.peak = std::max(check.peak, level);
      check.clipped += (sample == 32767 || sample == -32768) ? 1 : 0;
      if (level > kAudibleLevel) {
        last_audible = now;
        (i % 2 == 0 ? check.left_audible : check.right_audible) = true;
      }
    }

    const std::uint8_t* ram = spc.smp_ram();
    const size_t directory = size_t(dsp.read(kDspDirectory)) << 8;
    for (int voice = 0; voice < 8; ++voice) {
      if (dsp.read(voice * 16 + kVoiceEnvelope) == 0) {
        continue;
      }
      const int source = dsp.read(voice * 16 + kVoiceSource);
      if (!source_valid.count(source)) {
        const size_t entry = directory + size_t(source) * 4;
        const size_t start = entry + 1 < kAramSize ? size_t(ram[entry] | ram[entry + 1] << 8) : kAramSize;
        source_valid[source] = BrrChainValid(ram, start);
      }
    }
    snapshots.emplace_back();
    std::copy(ram, ram + kDriverRamBytes, snapshots.back().begin());

    if (last_audible >= 0 && now - last_audible >= kSilenceForEnd) {
      ended_silent = true;
      break;
    }
  }

  check.audible_until = std::max(0.0, last_audible);
  if (ended_silent) {
    check.loop = LoopKind::kOneShot;
  } else if (check.emulator_ok && last_audible >= 0) {
    const size_t first = size_t(kSettle / kChunkSeconds);
    const size_t window = size_t(kMovingWindow / kChunkSeconds);
    const size_t minimum = size_t(kMinimumLoop / kChunkSeconds);
    size_t moving = 0;
    size_t looping = 0;
    size_t longest = 0;
    for (const auto& values : SequencePointerSeries(snapshots, first, set.songs[song_id].data.size())) {
      if (!StillMoving(values, window)) {
        continue;
      }
      ++moving;
      if (const size_t period = ConfirmedPeriod(values, minimum)) {
        ++looping;
        longest = std::max(longest, period);
      }
    }
    if (moving > 0 && looping == moving) {
      check.loop = LoopKind::kLooping;
      check.loop_seconds = double(longest) * kChunkSeconds;
    }
  }
  for (const auto& [source, valid] : source_valid) {
    check.sources.push_back(source);
    if (!valid) {
      check.bad_sources.push_back(source);
    }
    check.uses_pack_samples = check.uses_pack_samples || source < kPackSources;
  }
  return check;
}

}  // namespace tmnt::classic
