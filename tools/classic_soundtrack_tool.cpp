// Developer CLI for the Classic Soundtrack. `validate`, `status`, `verify-all` and `test-playback` are read-only;
// `import` and `remove` write only under the given user data folder; `render` writes the one WAV file it is given.
// It prints metadata, never ROM or soundtrack bytes, and never plays audio.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/known_roms.h"
#include "classic_soundtrack/music_streams.h"
#include "classic_soundtrack/playback_check.h"
#include "classic_soundtrack/snes_rom.h"
#include "classic_soundtrack/soundtrack_stream.h"
#include "classic_soundtrack/xma_provider.h"

namespace fs = std::filesystem;
using namespace tmnt::classic;

namespace {

constexpr char kDefaultRomFolder[] = "game/snes";
constexpr double kVerifySeconds = 360.0;
constexpr double kQuickSeconds = 30.0;
constexpr double kMaxRenderSeconds = 600.0;
constexpr std::uint32_t kRenderRate = 44100;

bool IsRomFile(const fs::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".sfc" || extension == ".smc" || extension == ".zip";
}

int Validate(const fs::path& folder) {
  std::vector<fs::path> roms;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_regular_file() && IsRomFile(entry.path())) {
      roms.push_back(entry.path());
    }
  }
  if (ec || roms.empty()) {
    std::printf("no .sfc or .smc files in %s\n", folder.string().c_str());
    return 2;
  }
  std::sort(roms.begin(), roms.end());

  int failures = 0;
  for (const fs::path& path : roms) {
    std::vector<std::uint8_t> bytes;
    std::printf("file: %s\n", path.filename().string().c_str());
    std::string detail;
    if (ReadRomFile(path, bytes, detail) != ImportFailure::kNone) {
      std::printf("  result: cannot read: %s\n", detail.c_str());
      ++failures;
      continue;
    }
    const RomAnalysis analysis = AnalyzeRom(bytes);
    const bool ok = analysis.failure == ImportFailure::kNone;
    std::printf("  title: %s\n  copier header: %s\n  normalized size: %zu\n", analysis.title.c_str(),
                analysis.copier_header ? "yes" : "no", bytes.size() - (analysis.copier_header ? kCopierHeaderSize : 0));
    std::printf("  sha256: %s\n", analysis.rom_sha256.c_str());
    std::printf("  soundtrack fingerprint: %s\n", analysis.fingerprint.c_str());
    if (ok) {
      std::printf("  region: %s\n  songs: %zu\n  packs: %zu\n", analysis.manifest.source_region.c_str(),
                  analysis.set.songs.size(), analysis.set.packs.size());
      std::printf("  soundset bytes: %zu\n", analysis.soundset_bytes.size());
    }
    std::printf("  result: %s%s%s\n", ok ? "valid" : ImportFailureText(analysis.failure), ok ? "" : ": ",
                ok ? "" : analysis.detail.c_str());
    failures += ok ? 0 : 1;
  }
  return failures == 0 ? 0 : 1;
}

int Import(const fs::path& rom, const fs::path& root) {
  const ImportResult result = ImportSoundtrack(rom, root);
  if (!result.ok) {
    std::printf("import failed: %s: %s\n", ImportFailureText(result.failure), result.detail.c_str());
    return 1;
  }
  std::printf("imported %u songs from %s ROM into %s\n", result.manifest.song_count,
              result.manifest.source_region.c_str(), SoundtrackFolder(root).string().c_str());
  return 0;
}

int Status(const fs::path& root) {
  const StatusResult status = QuerySoundtrackStatus(root);
  std::printf("status: %s%s%s\n", SoundtrackStatusText(status.status), status.detail.empty() ? "" : ": ",
              status.detail.c_str());
  return status.status == SoundtrackStatus::kReady ? 0 : 1;
}

int Remove(const fs::path& root) {
  std::string error;
  if (!RemoveSoundtrack(root, error)) {
    std::printf("remove failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("removed %s\n", SoundtrackFolder(root).string().c_str());
  return 0;
}

bool LoadReady(const fs::path& root, SoundSet& set) {
  const StatusResult status = QuerySoundtrackStatus(root, &set);
  if (status.status != SoundtrackStatus::kReady) {
    std::printf("status: %s%s%s\n", SoundtrackStatusText(status.status), status.detail.empty() ? "" : ": ",
                status.detail.c_str());
    return false;
  }
  return true;
}

std::string Join(const std::vector<int>& values) {
  std::string text;
  for (const int value : values) {
    text += (text.empty() ? "" : ",") + std::to_string(value);
  }
  return text.empty() ? "-" : text;
}

void PrintCheck(const SoundSet& set, const PlaybackCheck& check) {
  const std::string label(SongLabel(check.song_id));
  std::printf("%2zu %-10s cmd %02X pack %2zu %-7s start=%s pcm=%llu L=%s R=%s peak=%5d clip=%llu %s",
              check.song_id, label.empty() ? "-" : label.c_str(), unsigned(set.songs[check.song_id].command),
              check.pack_id, LoopKindText(check.loop),
              check.start == SnesSoundtrackPlayer::StartResult::kOk ? "ok" : StartResultText(check.start),
              (unsigned long long)check.frames, check.left_audible ? "y" : "n", check.right_audible ? "y" : "n",
              check.peak, (unsigned long long)check.clipped, check.passed() ? "PASS" : "FAIL");
  if (check.loop == LoopKind::kLooping) {
    std::printf(" loop=%.1fs", check.loop_seconds);
  } else if (check.loop == LoopKind::kOneShot) {
    std::printf(" ends=%.1fs", check.audible_until);
  }
  if (check.emulator_error) {
    std::printf(" emulator=\"%s\" at %.1fs", check.emulator_error, double(check.frames) / SnesSoundtrackPlayer::kSampleRate);
  }
  const std::string bad = check.bad_sources.empty() ? std::string() : " BAD=" + Join(check.bad_sources);
  std::printf(" srcn=%s%s%s\n", Join(check.sources).c_str(), check.uses_pack_samples ? " (uses pack)" : "",
              bad.c_str());
}

int VerifyAll(const fs::path& root) {
  SoundSet set;
  if (!LoadReady(root, set)) {
    return 1;
  }
  int failures = 0;
  size_t counts[3] = {};
  for (size_t song = 0; song < set.songs.size(); ++song) {
    const PlaybackCheck check = CheckSong(set, song, kMusicPack, kVerifySeconds);
    PrintCheck(set, check);
    failures += check.passed() ? 0 : 1;
    ++counts[int(check.loop)];
  }
  for (const MusicStream& stream : kMusicStreams) {
    const bool passed = CheckSong(set, stream.song_id, kMusicPack, kQuickSeconds).passed();
    std::printf("music %-34.*s -> %2u %-10.*s %s\n", int(stream.context.size()), stream.context.data(),
                unsigned(stream.song_id), int(SongLabel(stream.song_id).size()), SongLabel(stream.song_id).data(),
                passed ? "PASS" : "FAIL");
    failures += passed ? 0 : 1;
  }
  std::printf("songs %zu: Looping %zu, OneShot %zu, Unknown %zu; %d failed\n", set.songs.size(), counts[0], counts[1],
              counts[2], failures);
  return failures == 0 ? 0 : 1;
}

int TestPlayback(const fs::path& root, int song) {
  SoundSet set;
  if (!LoadReady(root, set)) {
    return 1;
  }
  if (song < 0 || size_t(song) >= set.songs.size()) {
    std::printf("song must be 0-%zu\n", set.songs.size() - 1);
    return 2;
  }
  const PlaybackCheck check = CheckSong(set, size_t(song), kMusicPack, kQuickSeconds);
  PrintCheck(set, check);
  return check.passed() ? 0 : 1;
}

void PutU32(std::ofstream& out, std::uint32_t value) {
  const char bytes[4] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
  out.write(bytes, 4);
}

bool WriteWav(const fs::path& output, const std::vector<std::int16_t>& pcm, std::uint32_t rate) {
  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  const std::uint32_t data_bytes = std::uint32_t(pcm.size() * 2);
  out.write("RIFF", 4);
  PutU32(out, 36 + data_bytes);
  out.write("WAVEfmt ", 8);
  PutU32(out, 16);
  const char format[4] = {1, 0, 2, 0};
  out.write(format, 4);
  PutU32(out, rate);
  PutU32(out, rate * 4);
  const char block[4] = {4, 0, 16, 0};
  out.write(block, 4);
  out.write("data", 4);
  PutU32(out, data_bytes);
  for (const std::int16_t sample : pcm) {
    const char bytes[2] = {char(sample & 0xFF), char((sample >> 8) & 0xFF)};
    out.write(bytes, 2);
  }
  return bool(out);
}

// The sample rate and pack the game would use for this song, so a render matches what the game plays.
struct RenderRequest {
  std::uint32_t rate = kRenderRate;
  bool native = false;
  size_t pack = kMusicPack;
};

bool ParseRenderOptions(int argc, char** argv, int first, const SoundSet& set, RenderRequest& request) {
  for (int i = first; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "native") {
      request.native = true;
      request.rate = SnesSoundtrackPlayer::kSampleRate;
    } else if (option == "pack" && i + 1 < argc) {
      request.pack = size_t(std::atoi(argv[++i]));
    } else if (option == "44100" || option == "48000") {
      request.rate = std::uint32_t(std::atoi(option.c_str()));
    } else {
      std::printf("unknown render option '%s'\n", option.c_str());
      return false;
    }
  }
  return request.pack < set.packs.size();
}

bool RenderPcm(const SoundSet& set, size_t song, const RenderRequest& request, size_t frames,
               std::vector<std::int16_t>& pcm, const char*& failure) {
  pcm.assign(frames * 2, 0);
  if (request.native) {
    SnesSoundtrackPlayer player;
    const auto started = player.Start(set, song, request.pack);
    failure = StartResultText(started);
    return started == SnesSoundtrackPlayer::StartResult::kOk && player.Render(pcm.data(), frames) == frames;
  }
  SoundtrackStream stream;
  const auto started = stream.Start(set, song, request.pack, request.rate);
  failure = StartResultText(started);
  return started == SnesSoundtrackPlayer::StartResult::kOk && stream.Render(pcm.data(), frames);
}

int Render(int argc, char** argv) {
  const fs::path root = argv[2];
  const int song = std::atoi(argv[3]);
  const double seconds = std::atof(argv[4]);
  const fs::path output = argv[5];
  if (output.extension() != ".wav" || seconds <= 0 || seconds > kMaxRenderSeconds) {
    std::printf("render needs a .wav output and 0 < seconds <= %.0f\n", kMaxRenderSeconds);
    return 2;
  }
  SoundSet set;
  if (!LoadReady(root, set)) {
    return 1;
  }
  if (song < 0 || size_t(song) >= set.songs.size()) {
    std::printf("song must be 0-%zu\n", set.songs.size() - 1);
    return 2;
  }
  RenderRequest request;
  if (!ParseRenderOptions(argc, argv, 6, set, request)) {
    return 2;
  }
  std::vector<std::int16_t> pcm;
  const char* failure = "";
  const auto begin = std::chrono::steady_clock::now();
  if (!RenderPcm(set, size_t(song), request, size_t(seconds * request.rate), pcm, failure)) {
    std::printf("cannot render song %d: %s\n", song, failure);
    return 1;
  }
  const double render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  if (!WriteWav(output, pcm, request.rate)) {
    std::printf("cannot write %s\n", output.string().c_str());
    return 1;
  }
  std::printf("rendered song %d (%.*s) pack %zu for %.1f s at %u Hz%s to %s (%.0f ms)\n", song,
              int(SongLabel(size_t(song)).size()), SongLabel(size_t(song)).data(), request.pack, seconds,
              unsigned(request.rate), request.native ? " (raw emulator output)" : "", output.string().c_str(), render_ms);
  return 0;
}

struct PcmStats {
  double rate = 0;
  size_t frames = 0;
  int peak = 0;
  size_t clipped = 0;
  double rms_left = 0;
  double rms_right = 0;
  double dc_left = 0;
  double dc_right = 0;
  double crossings_per_second = 0;
  double dominant_hz = 0;
};

// Strongest bin of the averaged power spectrum of the left channel, 8192-point blocks.
double DominantFrequency(const std::vector<std::int16_t>& pcm, double rate) {
  constexpr size_t kSize = 8192;
  const size_t frames = pcm.size() / 2;
  std::vector<double> power(kSize / 2, 0.0);
  const double pi = 3.14159265358979323846;
  for (size_t at = 0; at + kSize <= frames; at += kSize) {
    std::vector<std::complex<double>> bins(kSize);
    for (size_t i = 0; i < kSize; ++i) {
      bins[i] = double(pcm[(at + i) * 2]) * (0.5 - 0.5 * std::cos(2 * pi * double(i) / double(kSize)));
    }
    for (size_t i = 1, j = 0; i < kSize; ++i) {
      size_t bit = kSize >> 1;
      for (; j & bit; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        std::swap(bins[i], bins[j]);
      }
    }
    for (size_t length = 2; length <= kSize; length <<= 1) {
      const std::complex<double> step(std::cos(-2 * pi / double(length)), std::sin(-2 * pi / double(length)));
      for (size_t i = 0; i < kSize; i += length) {
        std::complex<double> w(1);
        for (size_t k = 0; k < length / 2; ++k) {
          const auto u = bins[i + k];
          const auto v = bins[i + k + length / 2] * w;
          bins[i + k] = u + v;
          bins[i + k + length / 2] = u - v;
          w *= step;
        }
      }
    }
    for (size_t k = 1; k < kSize / 2; ++k) {
      power[k] += std::norm(bins[k]);
    }
  }
  size_t best = 1;
  for (size_t k = 2; k < kSize / 2; ++k) {
    best = power[k] > power[best] ? k : best;
  }
  return double(best) * rate / double(kSize);
}

PcmStats Measure(const std::vector<std::int16_t>& pcm, double rate) {
  PcmStats stats;
  stats.rate = rate;
  stats.frames = pcm.size() / 2;
  double sum[2] = {0, 0};
  double square[2] = {0, 0};
  size_t crossings = 0;
  for (size_t i = 0; i < pcm.size(); ++i) {
    const int sample = pcm[i];
    const size_t channel = i % 2;
    sum[channel] += sample;
    square[channel] += double(sample) * sample;
    stats.peak = std::max(stats.peak, std::abs(sample));
    stats.clipped += (sample == 32767 || sample == -32768) ? 1 : 0;
    if (channel == 0 && i >= 2 && (pcm[i] >= 0) != (pcm[i - 2] >= 0)) {
      ++crossings;
    }
  }
  const double n = double(std::max<size_t>(stats.frames, 1));
  stats.dc_left = sum[0] / n;
  stats.dc_right = sum[1] / n;
  stats.rms_left = std::sqrt(square[0] / n);
  stats.rms_right = std::sqrt(square[1] / n);
  stats.crossings_per_second = double(crossings) / (n / rate);
  stats.dominant_hz = DominantFrequency(pcm, rate);
  return stats;
}

void PrintStats(const char* label, const PcmStats& stats) {
  std::printf("  %-34s %6.0f Hz, 2 ch, %9zu frames (%.2f s), peak %5d, clipped %zu, RMS %.0f/%.0f, DC %.1f/%.1f, "
              "zero crossings %.0f/s, dominant %.0f Hz\n",
              label, stats.rate, stats.frames, double(stats.frames) / stats.rate, stats.peak, stats.clipped,
              stats.rms_left, stats.rms_right, stats.dc_left, stats.dc_right, stats.crossings_per_second,
              stats.dominant_hz);
}

void PrintDsp(double at, const DspSnapshot& dsp) {
  std::printf("  t=%.1f s: MVOL %02X/%02X EVOL %02X/%02X EFB %02X FLG %02X DIR %02X ESA %02X EDL %02X EON %02X  FIR",
              at, dsp.main_left & 0xFF, dsp.main_right & 0xFF, dsp.echo_left & 0xFF, dsp.echo_right & 0xFF,
              dsp.echo_feedback, dsp.flags, dsp.directory, dsp.echo_start, dsp.echo_delay, dsp.echo_enable);
  for (const int coefficient : dsp.fir) {
    std::printf(" %02X", coefficient & 0xFF);
  }
  std::printf("\n");
  for (size_t v = 0; v < dsp.voices.size(); ++v) {
    const VoiceSnapshot& voice = dsp.voices[v];
    if (voice.envelope == 0) {
      continue;
    }
    std::printf("    voice %zu: SRCN %02X pitch %04X vol %d/%d ADSR %02X%02X GAIN %02X envelope %02X%s%s\n", v,
                voice.source, voice.pitch, voice.volume_left, voice.volume_right, voice.adsr1, voice.adsr2, voice.gain,
                voice.envelope, voice.pitch_modulation ? " PMON" : "", voice.noise ? " NOISE" : "");
  }
}

// One-song developer report on the whole path: the emulator at its native rate, the production resampler, and the
// provider's own callback sizes. It prints statistics only; --wav-style output is the separate render command.
int Analyze(const fs::path& root, int song, double seconds) {
  if (seconds < 2 || seconds > 120) {
    std::printf("analyze needs 2 <= seconds <= 120\n");
    return 2;
  }
  SoundSet set;
  if (!LoadReady(root, set)) {
    return 1;
  }
  if (song < 0 || size_t(song) >= set.songs.size()) {
    std::printf("song must be 0-%zu\n", set.songs.size() - 1);
    return 2;
  }
  const size_t pack = kMusicPack;
  std::printf("song %d %.*s, command %02X, pack %zu\n", song, int(SongLabel(size_t(song)).size()),
              SongLabel(size_t(song)).data(), set.songs[size_t(song)].command, pack);

  SnesSoundtrackPlayer player;
  if (player.Start(set, size_t(song), pack) != SnesSoundtrackPlayer::StartResult::kOk) {
    std::printf("cannot start song %d\n", song);
    return 1;
  }
  const size_t native_frames = size_t(seconds * SnesSoundtrackPlayer::kSampleRate);
  std::vector<std::int16_t> native(native_frames * 2);
  std::vector<DspSnapshot> snapshots;
  std::vector<double> snapshot_times;
  std::map<int, std::pair<int, int>> pitch_range;
  std::set<int> sources;
  for (size_t done = 0; done < native_frames; done += 512) {
    const size_t count = std::min<size_t>(512, native_frames - done);
    if (player.Render(native.data() + done * 2, count) != count) {
      std::printf("the emulator stopped at %.2f s\n", double(done) / SnesSoundtrackPlayer::kSampleRate);
      return 1;
    }
    const DspSnapshot dsp = ReadDsp(player);
    for (const VoiceSnapshot& voice : dsp.voices) {
      if (voice.envelope == 0 || voice.pitch == 0) {
        continue;
      }
      sources.insert(voice.source);
      auto [it, added] = pitch_range.try_emplace(voice.source, voice.pitch, voice.pitch);
      if (!added) {
        it->second.first = std::min(it->second.first, voice.pitch);
        it->second.second = std::max(it->second.second, voice.pitch);
      }
    }
    for (const double mark : {2.0, 10.0}) {
      const size_t at = size_t(mark * SnesSoundtrackPlayer::kSampleRate);
      if (done <= at && at < done + 512 && mark < seconds) {
        snapshots.push_back(dsp);
        snapshot_times.push_back(mark);
      }
    }
  }
  std::printf("checkpoints (statistics only):\n");
  PrintStats("A raw emulator output", Measure(native, SnesSoundtrackPlayer::kSampleRate));

  std::vector<std::int16_t> production;
  for (const std::uint32_t rate : {44100u, 48000u}) {
    RenderRequest request;
    request.rate = rate;
    request.pack = pack;
    const char* failure = "";
    if (!RenderPcm(set, size_t(song), request, size_t(seconds * rate), production, failure)) {
      std::printf("cannot render at %u Hz: %s\n", unsigned(rate), failure);
      return 1;
    }
    char label[64];
    std::snprintf(label, sizeof(label), "B after the project resampler");
    PrintStats(label, Measure(production, rate));
    if (rate != 44100) {
      continue;
    }
    // C: the provider asked for 512-frame blocks, as the XMA decoder asks.
    const std::vector<std::int16_t> expected = production;
    MusicStream stream{0x1, 4096, 0, "analysis", std::uint8_t(song)};
    std::vector<std::uint8_t> buffer(4096, 0x5A);
    stream.fingerprint = XmaStreamFingerprint(buffer.data(), std::uint32_t(buffer.size()));
    const std::array<MusicStream, 1> streams{stream};
    ClassicSoundtrackProvider provider(set, nullptr, streams);
    const std::int32_t id = provider.Open(1, buffer.data(), std::uint32_t(buffer.size()), rate, 2);
    std::vector<std::int16_t> supplied(expected.size());
    size_t offset = 0;
    bool ok = id >= 0;
    while (ok && offset < expected.size() / 2) {
      const size_t count = std::min<size_t>(512, expected.size() / 2 - offset);
      ok = provider.Render(id, supplied.data() + offset * 2, std::uint32_t(count)) == kRenderReplaced;
      offset += count;
    }
    if (!ok) {
      std::printf("  C provider callbacks failed\n");
      return 1;
    }
    PrintStats("C as supplied to the XMA provider", Measure(supplied, rate));
    std::printf("  C is %s B\n", supplied == expected ? "identical to" : "DIFFERENT from");
  }

  std::printf("DSP state:\n");
  for (size_t i = 0; i < snapshots.size(); ++i) {
    PrintDsp(snapshot_times[i], snapshots[i]);
  }
  std::printf("keyed samples (pitch 1000 = the sample's own 32 kHz rate):\n");
  for (const int source : sources) {
    const SampleInfo info = DescribeSample(player, source);
    std::printf("  SRCN %02X: start %04zX loop %04zX, %zu blocks, %s%s, %zu blocks over range 12, pitch %04X-%04X\n",
                source, info.start, info.loop, info.blocks, info.ended ? "ends" : "NO END", info.loops ? " and loops" : "",
                info.range_over_12, pitch_range[source].first, pitch_range[source].second);
  }
  return 0;
}

void Usage() {
  std::printf(
      "usage:\n"
      "  classic_soundtrack_tool validate [rom_folder]      read-only, default %s\n"
      "  classic_soundtrack_tool import <rom> <user_data>   writes <user_data>/%s\n"
      "  classic_soundtrack_tool status <user_data>\n"
      "  classic_soundtrack_tool remove <user_data>\n"
      "  classic_soundtrack_tool verify-all <user_data>      every song on the emulated driver, no audio output\n"
      "  classic_soundtrack_tool test-playback <user_data> <song_id>\n"
      "  classic_soundtrack_tool render <user_data> <song_id> <seconds> <output.wav> [native|44100|48000] [pack N]\n"
      "      explicit developer export; native is the raw 32000 Hz emulator output, the default is the game's path at 44100 Hz\n"
      "  classic_soundtrack_tool analyze <user_data> <song_id> <seconds>   statistics for the raw, resampled and provider PCM\n",
      kDefaultRomFolder, kSoundtrackFolder);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  if (command == "validate" && argc <= 3) {
    return Validate(argc == 3 ? argv[2] : kDefaultRomFolder);
  }
  if (command == "import" && argc == 4) {
    return Import(argv[2], argv[3]);
  }
  if (command == "status" && argc == 3) {
    return Status(argv[2]);
  }
  if (command == "remove" && argc == 3) {
    return Remove(argv[2]);
  }
  if (command == "verify-all" && argc == 3) {
    return VerifyAll(argv[2]);
  }
  if (command == "test-playback" && argc == 4) {
    return TestPlayback(argv[2], std::atoi(argv[3]));
  }
  if (command == "render" && argc >= 6) {
    return Render(argc, argv);
  }
  if (command == "analyze" && argc == 5) {
    return Analyze(argv[2], std::atoi(argv[3]), std::atof(argv[4]));
  }
  Usage();
  return 2;
}
