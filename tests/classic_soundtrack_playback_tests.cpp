// Classic Soundtrack playback: SPC driver boot and command handshake, PCM, resampler, music stream table, XMA
// provider, no ROM at playback, no filesystem or allocation on the audio path. Synthetic SPC700 fixtures only.
// Run: tmnt_classic_soundtrack_playback_tests

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Snes_Spc.h"
#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack/music_streams.h"
#include "classic_soundtrack/playback_check.h"
#include "classic_soundtrack/resampler.h"
#include "classic_soundtrack/soundset.h"
#include "classic_soundtrack/soundtrack_stream.h"
#include "classic_soundtrack/spc_player.h"
#include "classic_soundtrack/xma_provider.h"
#include "updater/sha256.h"

namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<long> g_allocations{0};
}  // namespace

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TMNT_SANITIZED_ALLOCATOR 1
#endif
#endif

// Sanitizers own operator new, so the allocation count only runs in the normal build.
#ifndef TMNT_SANITIZED_ALLOCATOR
void* operator new(std::size_t size) {
  if (g_count_allocations.load()) {
    ++g_allocations;
  }
  if (void* p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

namespace fs = std::filesystem;
using namespace tmnt::classic;
using Bytes = std::vector<std::uint8_t>;
using Pcm = std::vector<std::int16_t>;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

// ---- Synthetic SPC700 driver -------------------------------------------------------------------------------------

enum class Driver { kPlays, kSilent, kNoEcho, kFaults };

Bytes DriverProgram(Driver kind) {
  Bytes p;
  const auto emit = [&](std::initializer_list<int> bytes) {
    for (const int b : bytes) {
      p.push_back(std::uint8_t(b));
    }
  };
  const auto dsp = [&](int reg, int value) { emit({0x8F, reg, 0xF2, 0x8F, value, 0xF3}); };
  const auto branch = [&](int opcode, size_t target) {
    emit({opcode, int(std::int8_t(int(target) - int(p.size() + 2))) & 0xFF});
  };
  emit({0xCD, 0xEF, 0xBD});  // MOV X,#$EF; MOV SP,X
  dsp(0x6C, 0x20);           // FLG: unmute, echo writes off
  dsp(0x5D, 0xFE);           // DIR
  dsp(0x0C, 0x7F);
  dsp(0x1C, 0x7F);
  dsp(0x00, 0x7F);           // voice 0 left louder than right
  dsp(0x01, 0x30);
  dsp(0x02, 0x00);
  dsp(0x03, 0x10);
  dsp(0x04, 0x00);
  dsp(0x05, 0x8F);
  dsp(0x06, 0xE0);
  const size_t loop = p.size();
  emit({0xE4, 0xF4});  // MOV A,$F4
  branch(0xF0, loop);  // BEQ loop
  if (kind != Driver::kNoEcho) {
    emit({0xC4, 0xF4});  // MOV $F4,A
  }
  emit({0x8F, 0x10, 0xF1});  // clear input ports 0 and 1
  if (kind == Driver::kFaults) {
    emit({0xFF});  // STOP
  }
  emit({0x68, 0x80});  // CMP A,#$80
  branch(0x90, loop);  // BCC loop
  if (kind == Driver::kPlays) {
    dsp(0x5C, 0x00);
    dsp(0x4C, 0x01);
  }
  branch(0x2F, loop);
  return p;
}

Bytes SquareBrr(size_t blocks) {
  Bytes brr;
  for (size_t i = 0; i < blocks; ++i) {
    brr.push_back(0xC3);  // range 12, loop, end
    for (const int byte : {0x77, 0x77, 0x77, 0x77, 0x99, 0x99, 0x99, 0x99}) {
      brr.push_back(std::uint8_t(byte));
    }
  }
  return brr;
}

SoundSet MakeSet(Driver kind = Driver::kPlays) {
  SoundSet set;
  set.boot.push_back({kDriverAddress, DriverProgram(kind)});
  set.boot.push_back({0x6100, SquareBrr(1)});
  set.boot.push_back({kDirectoryAddress, Bytes{0x00, 0x61, 0x00, 0x61}});
  for (size_t i = 0; i < kPackCount; ++i) {
    StagePack pack;
    pack.command = std::uint8_t(0x70 + i);
    pack.blocks.push_back({kPackSamplesAddress, SquareBrr(3)});
    pack.blocks.push_back({kDirectoryAddress, Bytes{0x00, 0x3E, 0x00, 0x3E, 0x09, 0x3E, 0x09, 0x3E, 0x12, 0x3E, 0x12, 0x3E}});
    set.packs.push_back(std::move(pack));
  }
  for (size_t i = 0; i < kSongCount; ++i) {
    Song song;
    song.command = i % 5 == 0 ? kSongCommand8Track : kSongCommand6Track;
    song.data = Bytes(16, std::uint8_t(i));
    song.pack_id = i < 10 ? std::uint16_t(i + 1) : kNoId;
    set.songs.push_back(std::move(song));
  }
  for (size_t i = 0; i < kStageCount; ++i) {
    set.stage_songs.push_back(i < 10 ? std::uint16_t(i) : kNoId);
  }
  return set;
}

int Peak(const Pcm& pcm, int channel) {
  int peak = 0;
  for (size_t i = size_t(channel); i < pcm.size(); i += 2) {
    peak = std::max(peak, std::abs(int(pcm[i])));
  }
  return peak;
}

// ---- Tests -------------------------------------------------------------------------------------------------------

void TestPlayer() {
  std::printf("SPC player: driver boot, port handshake, PCM, stop, reset, song change, failures\n");
  const SoundSet set = MakeSet();
  CHECK(ValidateSoundSet(set) == SoundSetError::kOk);
  CHECK(SnesSoundtrackPlayer::kSampleRate == 32000);

  SnesSoundtrackPlayer player;
  CHECK(!player.playing());
  CHECK(player.Start(set, 3, 2) == SnesSoundtrackPlayer::StartResult::kOk);
  CHECK(player.playing());
  CHECK(player.emulator().read_port(0, 0) == set.songs[3].command);
  Pcm first(4096 * 2);
  CHECK(player.Render(first.data(), 4096) == 4096);
  CHECK(Peak(first, 0) > 1000 && Peak(first, 1) > 100);
  CHECK(Peak(first, 0) > Peak(first, 1));
  CHECK(player.emulator().smp_ram()[kPackSamplesAddress] == 0xC3);
  CHECK(player.emulator().smp_ram()[kSongAddress] == 3);

  CHECK(player.Start(set, 3, 2) == SnesSoundtrackPlayer::StartResult::kOk);
  Pcm again(4096 * 2);
  player.Render(again.data(), 4096);
  CHECK(again == first);
  SnesSoundtrackPlayer other;
  other.Start(set, 3, 2);
  Pcm other_pcm(4096 * 2);
  other.Render(other_pcm.data(), 4096);
  CHECK(other_pcm == first);

  CHECK(player.Start(set, 7, 0) == SnesSoundtrackPlayer::StartResult::kOk);
  CHECK(player.emulator().smp_ram()[kSongAddress] == 7);
  CHECK(player.emulator().smp_ram()[kPackSamplesAddress] == 0xC3);

  player.Stop();
  CHECK(!player.playing());
  Pcm stopped(64 * 2, 1);
  CHECK(player.Render(stopped.data(), 64) == 0);

  CHECK(player.Start(set, kSongCount, 0) == SnesSoundtrackPlayer::StartResult::kBadSong);
  CHECK(player.Start(set, 0, kPackCount) == SnesSoundtrackPlayer::StartResult::kBadPack);
  CHECK(!player.playing());

  SnesSoundtrackPlayer mute;
  CHECK(mute.Start(MakeSet(Driver::kNoEcho), 0, 0) == SnesSoundtrackPlayer::StartResult::kNoAcknowledge);
  CHECK(!mute.playing());
  SnesSoundtrackPlayer faulty;
  CHECK(faulty.Start(MakeSet(Driver::kFaults), 0, 0) != SnesSoundtrackPlayer::StartResult::kOk);
  CHECK(!faulty.playing() && faulty.error() != nullptr);
  CHECK(std::string(StartResultText(SnesSoundtrackPlayer::StartResult::kNoAcknowledge)).size() > 5);

  SnesSoundtrackPlayer silent;
  CHECK(silent.Start(MakeSet(Driver::kSilent), 0, 0) == SnesSoundtrackPlayer::StartResult::kOk);
  Pcm quiet(4096 * 2, 1);
  silent.Render(quiet.data(), 4096);
  CHECK(Peak(quiet, 0) == 0 && Peak(quiet, 1) == 0);
}

void TestPlaybackCheck() {
  std::printf("playback check: audible, silent, one-shot, sources\n");
  const SoundSet set = MakeSet();
  const PlaybackCheck plays = CheckSong(set, 2, 3, 3.0);
  CHECK(plays.passed());
  CHECK(plays.left_audible && plays.right_audible && plays.frames == 3 * 32000 / 512 * 512);
  CHECK(plays.sources.size() == 1 && plays.sources[0] == 0 && plays.uses_pack_samples && plays.bad_sources.empty());
  const PlaybackCheck silent = CheckSong(MakeSet(Driver::kSilent), 2, 3, 3.0);
  CHECK(!silent.passed() && !silent.left_audible && silent.loop == LoopKind::kUnknown);
  const PlaybackCheck failed = CheckSong(MakeSet(Driver::kNoEcho), 2, 3, 1.0);
  CHECK(!failed.passed() && failed.start == SnesSoundtrackPlayer::StartResult::kNoAcknowledge);

  SoundSet bad_sample = MakeSet();
  bad_sample.packs[3].blocks[1].data[0] = 0xF8;
  bad_sample.packs[3].blocks[1].data[1] = 0xFF;
  const PlaybackCheck bad = CheckSong(bad_sample, 2, 3, 1.0);
  CHECK(!bad.passed() && bad.bad_sources.size() == 1);
  CHECK(std::string(LoopKindText(LoopKind::kOneShot)) == "OneShot");
}

void TestResampler() {
  std::printf("resampler: ratio, exact counts, continuity, stereo, reset, silence, short input\n");
  StereoResampler resampler;
  CHECK(!resampler.Configure(0, 44100) && !resampler.Configure(32000, 0));
  CHECK(resampler.Configure(32000, 44100));
  CHECK(resampler.input_step() == 320 && resampler.output_step() == 441);
  CHECK(resampler.Configure(32000, 48000) && resampler.input_step() == 2 && resampler.output_step() == 3);

  const auto constant = [](std::int16_t left, std::int16_t right) {
    return [left, right](std::int16_t* dst, size_t frames) {
      for (size_t i = 0; i < frames; ++i) {
        dst[i * 2] = left;
        dst[i * 2 + 1] = right;
      }
      return frames;
    };
  };

  resampler.Configure(32000, 44100);
  const size_t outputs = 44100 * 600;
  Pcm block(441 * 2);
  for (size_t done = 0; done < outputs; done += 441) {
    resampler.Process(block.data(), 441, constant(0, 0));
  }
  CHECK(resampler.consumed_frames() == 32000ull * 600);

  resampler.Configure(32000, 44100);
  Pcm out(1000 * 2);
  CHECK(resampler.Process(out.data(), 1000, constant(1000, -500)));
  bool steady = true;
  for (size_t i = 8; i < 1000; ++i) {
    steady = steady && out[i * 2] == 1000 && out[i * 2 + 1] == -500;
  }
  CHECK(steady);

  resampler.Configure(32000, 44100);
  long long next = 0;
  const auto ramp = [&next](std::int16_t* dst, size_t frames) {
    for (size_t i = 0; i < frames; ++i, ++next) {
      dst[i * 2] = std::int16_t(next % 20000);
      dst[i * 2 + 1] = std::int16_t(-(next % 20000));
    }
    return frames;
  };
  Pcm ramp_out(4000 * 2);
  resampler.Process(ramp_out.data(), 4000, ramp);
  bool continuous = true;
  for (size_t i = 8; i < 4000; ++i) {
    const int step = ramp_out[i * 2] - ramp_out[(i - 1) * 2];
    continuous = continuous && step >= 0 && step <= 1 && ramp_out[i * 2] == -ramp_out[i * 2 + 1];
  }
  CHECK(continuous);

  resampler.Configure(32000, 44100);
  next = 0;
  Pcm a(777 * 2);
  resampler.Process(a.data(), 777, ramp);
  resampler.Reset();
  next = 0;
  Pcm b(777 * 2);
  resampler.Process(b.data(), 777, ramp);
  CHECK(a == b);
  CHECK(resampler.consumed_frames() == (777ull * 320) / 441 + 0 || resampler.consumed_frames() == (777ull * 320) / 441 + 1);

  resampler.Configure(32000, 32000);
  next = 0;
  Pcm identity(100 * 2);
  resampler.Process(identity.data(), 100, ramp);
  bool delayed = true;
  for (size_t i = 3; i < 100; ++i) {
    delayed = delayed && identity[i * 2] == std::int16_t(i - 3);
  }
  CHECK(delayed);

  resampler.Configure(32000, 44100);
  Pcm silence(500 * 2, 7);
  resampler.Process(silence.data(), 500, constant(0, 0));
  CHECK(Peak(silence, 0) == 0 && Peak(silence, 1) == 0);

  resampler.Configure(32000, 44100);
  Pcm starved(500 * 2);
  CHECK(!resampler.Process(starved.data(), 500, [](std::int16_t*, size_t) { return size_t(0); }));
  CHECK(Peak(starved, 0) == 0);

  SoundtrackStream stream;
  const SoundSet set = MakeSet();
  CHECK(stream.Start(set, 1, 0, 44100) == SnesSoundtrackPlayer::StartResult::kOk);
  Pcm host(512 * 2);
  CHECK(stream.Render(host.data(), 512));
  CHECK(stream.resampler().output_step() == 441);
  stream.Stop();
  CHECK(!stream.Render(host.data(), 512));
  CHECK(stream.Start(set, 1, 0, 0) != SnesSoundtrackPlayer::StartResult::kOk);
}

void TestMusicStreams() {
  std::printf("music streams: fingerprint, table uniqueness, mapping\n");
  CHECK(XmaStreamFingerprint(nullptr, 0) == 0x4D25767F9DCE13F5ull);
  const std::string hello = "hello";
  CHECK(XmaStreamFingerprint(reinterpret_cast<const std::uint8_t*>(hello.data()), 5) == 0x578BE62969F42978ull);
  Bytes pattern;
  for (int i = 0; i < 20; ++i) {
    for (int b = 0; b < 256; ++b) {
      pattern.push_back(std::uint8_t(b));
    }
  }
  CHECK(XmaStreamFingerprint(pattern.data(), std::uint32_t(pattern.size())) == 0xDDDE4B48B3A21E99ull);
  Bytes tail_changed = pattern;
  tail_changed.back() ^= 1;
  CHECK(XmaStreamFingerprint(tail_changed.data(), std::uint32_t(tail_changed.size())) ==
        XmaStreamFingerprint(pattern.data(), std::uint32_t(pattern.size())));
  Bytes head_changed = pattern;
  head_changed[100] ^= 1;
  CHECK(XmaStreamFingerprint(head_changed.data(), std::uint32_t(head_changed.size())) !=
        XmaStreamFingerprint(pattern.data(), std::uint32_t(pattern.size())));

  std::set<std::uint64_t> fingerprints;
  std::set<std::uint32_t> keys;
  for (const MusicStream& stream : kMusicStreams) {
    fingerprints.insert(stream.fingerprint);
    keys.insert(stream.wave_key);
    CHECK(stream.song_id < kSongCount && !SongLabel(stream.song_id).empty());
    CHECK(stream.bytes % 2048 == 0 && stream.bytes > 0);
    CHECK(FindMusicStream(kMusicStreams, stream.fingerprint, stream.bytes) == &stream);
    CHECK(FindMusicStream(kMusicStreams, stream.fingerprint, stream.bytes + 2048) == nullptr);
  }
  CHECK(fingerprints.size() == kMusicStreams.size() && keys.size() == kMusicStreams.size());
  CHECK(FindMusicStream(kMusicStreams, 0x0123456789ABCDEFull, 2048) == nullptr);
  CHECK(kMusicStreams[0].context == "StartMenu" && kMusicPack == 0);
  const std::string tool = ReadText(fs::path(TMNT_ROOT_DIR) / "tools" / "xma_music_fingerprints.py");
  for (const MusicStream& stream : kMusicStreams) {
    char key[16];
    std::snprintf(key, sizeof(key), "0x%08X", unsigned(stream.wave_key));
    CHECK(Has(tool, key));
  }
  CHECK(Has(tool, "HASHED_BYTES = 4096"));
}

std::vector<Bytes> g_buffers;
std::vector<MusicStream> g_streams;
std::vector<std::string> g_warnings;

void Warn(const char* message) {
  g_warnings.emplace_back(message);
}

void MakeStreams() {
  g_buffers.clear();
  g_streams.clear();
  const std::uint8_t songs[] = {3, 12, 20, 5};
  for (size_t i = 0; i < std::size(songs); ++i) {
    Bytes buffer(8192, std::uint8_t(0x40 + i));
    g_buffers.push_back(buffer);
    g_streams.push_back({XmaStreamFingerprint(buffer.data(), std::uint32_t(buffer.size())), std::uint32_t(buffer.size()),
                         std::uint32_t(0x100 + i), "synthetic", songs[i]});
  }
}

std::int32_t OpenBuffer(ClassicSoundtrackProvider& provider, std::uint32_t context, size_t buffer,
                        std::uint32_t channels = 2) {
  return provider.Open(context, g_buffers[buffer].data(), std::uint32_t(g_buffers[buffer].size()), 44100, channels);
}

void TestProvider() {
  std::printf("XMA provider: music only, continuity, packs, fallback, no retry, slots, callbacks\n");
  MakeStreams();
  g_warnings.clear();
  ClassicSoundtrackProvider provider(MakeSet(), Warn, g_streams);

  Bytes sfx(4096, 0x11);
  CHECK(provider.Open(1, sfx.data(), std::uint32_t(sfx.size()), 44100, 2) == ClassicSoundtrackProvider::kNoStream);
  Bytes voice(2048 * 3, 0x22);
  CHECK(provider.Open(1, voice.data(), std::uint32_t(voice.size()), 44100, 1) == ClassicSoundtrackProvider::kNoStream);
  CHECK(OpenBuffer(provider, 1, 0, 1) == ClassicSoundtrackProvider::kNoStream);
  CHECK(provider.Open(1, nullptr, 0, 44100, 2) == ClassicSoundtrackProvider::kNoStream);
  CHECK(provider.active_streams() == 0);

  const std::int32_t stage = OpenBuffer(provider, 7, 0);
  CHECK(stage >= 0 && provider.active_streams() == 1);
  CHECK(OpenBuffer(provider, 7, 0) == stage);
  CHECK(provider.active_streams() == 1);
  Pcm frames(512 * 2);
  CHECK(provider.Render(stage, frames.data(), 512) == kRenderReplaced);
  CHECK(Peak(frames, 0) > 1000);

  const std::int32_t boss = OpenBuffer(provider, 8, 1);
  CHECK(boss >= 0 && boss != stage && provider.active_streams() == 2);
  const std::int32_t menu = OpenBuffer(provider, 9, 2);
  CHECK(menu >= 0);

  provider.Close(boss);
  CHECK(provider.Render(boss, frames.data(), 512) == kRenderFailed);
  CHECK(provider.active_streams() == 2);
  const std::int32_t other_context = OpenBuffer(provider, 10, 0);
  CHECK(other_context >= 0 && other_context != stage);
  provider.Close(stage);
  provider.Close(menu);
  provider.Close(other_context);
  provider.Close(-1);
  provider.Close(99);
  CHECK(provider.active_streams() == 0);
  CHECK(provider.Render(-1, frames.data(), 512) == kRenderFailed && provider.Render(99, frames.data(), 512) == kRenderFailed);
  CHECK(g_warnings.empty());

  std::vector<std::int32_t> opened;
  for (std::uint32_t context = 0; context < ClassicSoundtrackProvider::kStreamSlots; ++context) {
    opened.push_back(OpenBuffer(provider, 100 + context, 0));
  }
  CHECK(std::count(opened.begin(), opened.end(), ClassicSoundtrackProvider::kNoStream) == 0);
  CHECK(OpenBuffer(provider, 200, 0) == ClassicSoundtrackProvider::kNoStream);
  CHECK(g_warnings.size() == 1);
  for (const std::int32_t stream : opened) {
    provider.Close(stream);
  }
  CHECK(OpenBuffer(provider, 200, 0) >= 0);

  g_warnings.clear();
  ClassicSoundtrackProvider broken(MakeSet(Driver::kNoEcho), Warn, g_streams);
  CHECK(OpenBuffer(broken, 1, 0) == ClassicSoundtrackProvider::kNoStream);
  CHECK(broken.song_failed(3) && g_warnings.size() == 1);
  CHECK(OpenBuffer(broken, 1, 0) == ClassicSoundtrackProvider::kNoStream);
  CHECK(g_warnings.size() == 1);
  CHECK(Has(g_warnings[0], "original music"));

  g_warnings.clear();
  SoundSet faulting = MakeSet();
  ClassicSoundtrackProvider renderer(std::move(faulting), Warn, g_streams);
  const std::int32_t ok = OpenBuffer(renderer, 1, 0);
  CHECK(ok >= 0 && renderer.Render(ok, frames.data(), 512) == kRenderReplaced);

  g_warnings.clear();
  ClassicSoundtrackProvider noted(MakeSet(), nullptr, g_streams, Warn);
  const std::int32_t first_note = OpenBuffer(noted, 1, 0);
  CHECK(first_note >= 0 && g_warnings.size() == 1 && Has(g_warnings[0], "plays SNES song 3"));
  CHECK(OpenBuffer(noted, 1, 0) == first_note && g_warnings.size() == 1);

  RexXmaPcmProvider callbacks = provider.Callbacks();
  CHECK(callbacks.user == &provider && callbacks.open && callbacks.render && callbacks.close);
  const std::int32_t via = callbacks.open(callbacks.user, 300, g_buffers[3].data(), std::uint32_t(g_buffers[3].size()), 48000, 2);
  CHECK(via >= 0 && callbacks.render(callbacks.user, via, frames.data(), 512) == kRenderReplaced);
  callbacks.close(callbacks.user, via);
  CHECK(callbacks.render(callbacks.user, via, frames.data(), 512) == kRenderFailed);
}

// ---- Audio contract and runtime switching -------------------------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;

// Positive-going zero crossings per second, interpolated, over frames [skip, size).
double MeasureFrequency(const Pcm& pcm, int channel, size_t skip, double rate) {
  double first = -1;
  double last = -1;
  size_t crossings = 0;
  const size_t frames = pcm.size() / 2;
  for (size_t i = std::max<size_t>(skip, 1); i < frames; ++i) {
    const double previous = pcm[(i - 1) * 2 + size_t(channel)];
    const double current = pcm[i * 2 + size_t(channel)];
    if (previous < 0 && current >= 0) {
      const double at = double(i - 1) + (0 - previous) / (current - previous);
      if (first < 0) {
        first = at;
      }
      last = at;
      ++crossings;
    }
  }
  return crossings < 2 ? 0 : double(crossings - 1) * rate / (last - first);
}

// Sine tones at the input rate: left at left_hz, right at right_hz, phase kept across calls.
struct ToneSource {
  double rate;
  double left_hz;
  double right_hz;
  double amplitude = 12000;
  size_t position = 0;
  size_t Pull(std::int16_t* dst, size_t frames) {
    for (size_t i = 0; i < frames; ++i, ++position) {
      const double t = double(position) / rate;
      dst[i * 2] = std::int16_t(std::lround(amplitude * std::sin(2 * kPi * left_hz * t)));
      dst[i * 2 + 1] = std::int16_t(std::lround(amplitude * std::sin(2 * kPi * right_hz * t)));
    }
    return frames;
  }
};

void TestNativeRate() {
  std::printf("native rate: the emulator makes 32000 frames per second and a known pitch comes out right\n");
  CHECK(Snes_Spc::sample_rate == 32000 && Snes_Spc::clock_rate == 1024000 && Snes_Spc::clocks_per_sample == 32);
  CHECK(SnesSoundtrackPlayer::kSampleRate == Snes_Spc::sample_rate);

  SnesSoundtrackPlayer player;
  CHECK(player.Start(MakeSet(), 0, 0) == SnesSoundtrackPlayer::StartResult::kOk);
  const size_t frames = 32000 * 4;
  Pcm pcm(frames * 2);
  CHECK(player.Render(pcm.data(), frames) == frames);
  // Pitch $1000 plays one sample per output frame; the sample is a 16-sample square cycle.
  const double hz = MeasureFrequency(pcm, 0, 4000, 32000);
  CHECK(std::abs(hz - 2000.0) < 2.0);
}

void TestResamplerTones() {
  std::printf("resampler tones: frequency, level and channels survive 32000/44100/48000\n");
  const std::pair<std::uint32_t, std::uint32_t> rates[] = {{32000, 44100}, {32000, 48000}, {44100, 48000}};
  for (const auto& [input, output] : rates) {
    for (const double hz : {440.0, 1000.0, 4000.0}) {
      StereoResampler resampler;
      CHECK(resampler.Configure(input, output));
      ToneSource source{double(input), hz, hz * 1.5};
      const size_t frames = size_t(output) * 3;
      Pcm out(frames * 2);
      size_t done = 0;
      const size_t sizes[] = {441, 512, 64, 1000, 777, 128};
      for (size_t round = 0; done < frames; ++round) {
        const size_t count = std::min(sizes[round % std::size(sizes)], frames - done);
        resampler.Process(out.data() + done * 2, count,
                          [&source](std::int16_t* dst, size_t n) { return source.Pull(dst, n); });
        done += count;
      }
      const double left = MeasureFrequency(out, 0, 2000, double(output));
      const double right = MeasureFrequency(out, 1, 2000, double(output));
      CHECK(std::abs(left - hz) / hz < 0.0005);
      CHECK(std::abs(right - hz * 1.5) / (hz * 1.5) < 0.0005);
      int peak = 0;
      for (size_t i = 2000; i < frames; ++i) {
        peak = std::max(peak, std::abs(int(out[i * 2])));
      }
      // 4 kHz at 32 kHz input has two samples per half cycle: Catmull-Rom loses a little, never more than 15%.
      CHECK(peak > 12000 * 85 / 100 && peak < 12000 * 102 / 100);
    }
  }
}

void TestCallbackBoundaries() {
  std::printf("callback boundaries: any callback size gives the same samples, no gap or repeat\n");
  for (const std::uint32_t output : {44100u, 48000u}) {
    const size_t frames = size_t(output);
    const auto render = [&](const std::vector<size_t>& sizes) {
      StereoResampler resampler;
      resampler.Configure(32000, output);
      ToneSource source{32000, 700, 1300};
      Pcm out(frames * 2);
      size_t done = 0;
      for (size_t round = 0; done < frames; ++round) {
        const size_t count = std::min(sizes[round % sizes.size()], frames - done);
        resampler.Process(out.data() + done * 2, count,
                          [&source](std::int16_t* dst, size_t n) { return source.Pull(dst, n); });
        done += count;
      }
      return out;
    };
    const Pcm whole = render({frames});
    CHECK(render({512}) == whole);
    CHECK(render({64, 448, 1, 4096}) == whole);
    CHECK(render({441, 1000, 3}) == whole);
  }

  SoundtrackStream stream;
  const SoundSet set = MakeSet();
  CHECK(stream.Start(set, 0, 0, 48000) == SnesSoundtrackPlayer::StartResult::kOk);
  Pcm one(48000 * 2);
  CHECK(stream.Render(one.data(), 48000));
  CHECK(stream.Start(set, 0, 0, 48000) == SnesSoundtrackPlayer::StartResult::kOk);
  Pcm many(48000 * 2);
  size_t done = 0;
  for (size_t round = 0; done < 48000; ++round) {
    const size_t count = std::min<size_t>(round % 2 ? 512 : 64, 48000 - done);
    CHECK(stream.Render(many.data() + done * 2, count));
    done += count;
  }
  CHECK(many == one);
}

void TestStereoFramesAndDrift() {
  std::printf("frames: the provider fills exactly the requested stereo frames at the stream rate, with no drift\n");
  MakeStreams();
  g_warnings.clear();
  for (const std::uint32_t rate : {44100u, 48000u}) {
    ClassicSoundtrackProvider provider(MakeSet(), Warn, g_streams);
    const std::int32_t stream = provider.Open(1, g_buffers[0].data(), std::uint32_t(g_buffers[0].size()), rate, 2);
    CHECK(stream >= 0);
    const size_t total = size_t(rate) * 5;
    Pcm pcm(total * 2, std::int16_t(12345));
    size_t done = 0;
    const size_t sizes[] = {512, 64, 192, 448, 128};
    for (size_t round = 0; done < total; ++round) {
      const size_t count = std::min(sizes[round % std::size(sizes)], total - done);
      CHECK(provider.Render(stream, pcm.data() + done * 2, std::uint32_t(count)) == kRenderReplaced);
      done += count;
    }
    size_t untouched = 0;
    for (const std::int16_t sample : pcm) {
      untouched += sample == 12345 ? 1 : 0;
    }
    CHECK(untouched == 0);
    const double hz = MeasureFrequency(pcm, 0, rate / 2, double(rate));
    CHECK(std::abs(hz - 2000.0) < 3.0);
    const double right = MeasureFrequency(pcm, 1, rate / 2, double(rate));
    CHECK(std::abs(right - 2000.0) < 3.0);
    CHECK(Peak(pcm, 0) > Peak(pcm, 1));
    provider.Close(stream);
  }

  StereoResampler resampler;
  for (const std::uint32_t output : {44100u, 48000u}) {
    resampler.Configure(32000, output);
    ToneSource source{32000, 500, 600};
    Pcm block(512 * 2);
    size_t produced = 0;
    while (produced + 512 <= size_t(output) * 600) {
      resampler.Process(block.data(), 512, [&source](std::int16_t* dst, size_t n) { return source.Pull(dst, n); });
      produced += 512;
    }
    const unsigned long long expected = (unsigned long long)produced * 32000 / output;
    CHECK(resampler.consumed_frames() + 2 >= expected && resampler.consumed_frames() <= expected + 2);
  }
}

void TestRuntimeSwitching() {
  std::printf("runtime switching: Off passes the original through, On restarts the current song, no allocation\n");
  MakeStreams();
  g_warnings.clear();
  ClassicSoundtrackProvider provider(MakeSet(), Warn, g_streams);
  Pcm frames(512 * 2);

  provider.SetReplacing(false);
  const std::int32_t stage = OpenBuffer(provider, 7, 0);
  CHECK(stage >= 0 && provider.active_streams() == 1 && provider.playing_streams() == 0);
  Bytes unknown(4096, 0x11);
  CHECK(provider.Open(1, unknown.data(), std::uint32_t(unknown.size()), 44100, 2) == ClassicSoundtrackProvider::kNoStream);
  std::fill(frames.begin(), frames.end(), std::int16_t(777));
  CHECK(provider.Render(stage, frames.data(), 512) == kRenderPassThrough);
  CHECK(frames[0] == 777 && frames.back() == 777 && provider.playing_streams() == 0);

  provider.SetReplacing(true);
  Pcm first(512 * 2);
  CHECK(provider.Render(stage, first.data(), 512) == kRenderReplaced);
  CHECK(provider.playing_streams() == 1 && Peak(first, 0) > 1000);
  Pcm later(512 * 2);
  for (int block = 0; block < 40; ++block) {
    provider.Render(stage, later.data(), 512);
  }

  provider.SetReplacing(false);
  std::fill(frames.begin(), frames.end(), std::int16_t(777));
  CHECK(provider.Render(stage, frames.data(), 512) == kRenderPassThrough && frames[100] == 777);
  CHECK(provider.playing_streams() == 0 && provider.active_streams() == 1);

  provider.SetReplacing(true);
  Pcm again(512 * 2);
  CHECK(provider.Render(stage, again.data(), 512) == kRenderReplaced);
  CHECK(again == first);

  provider.SetReplacing(false);
  CHECK(provider.Render(stage, frames.data(), 512) == kRenderPassThrough);
  const std::int32_t boss = OpenBuffer(provider, 8, 1);
  CHECK(boss >= 0 && provider.playing_streams() == 0);
  const std::int32_t menu = OpenBuffer(provider, 9, 2);
  CHECK(menu >= 0);
  provider.SetReplacing(true);
  CHECK(provider.Render(boss, frames.data(), 512) == kRenderReplaced);

  g_allocations = 0;
  g_count_allocations = true;
  for (int round = 0; round < 6; ++round) {
    provider.SetReplacing(round % 2 == 0);
    provider.Render(stage, frames.data(), 512);
    provider.Render(menu, frames.data(), 512);
  }
  g_count_allocations = false;
  CHECK(g_allocations.load() == 0);

  provider.Close(stage);
  provider.Close(boss);
  provider.Close(menu);
  CHECK(provider.active_streams() == 0 && provider.playing_streams() == 0);
  CHECK(g_warnings.empty());

  RexXmaPcmProvider callbacks = provider.Callbacks();
  provider.SetReplacing(false);
  const std::int32_t via = callbacks.open(callbacks.user, 5, g_buffers[0].data(), std::uint32_t(g_buffers[0].size()), 44100, 2);
  CHECK(via >= 0 && callbacks.render(callbacks.user, via, frames.data(), 512) == kRenderPassThrough);
  provider.SetReplacing(true);
  CHECK(callbacks.render(callbacks.user, via, frames.data(), 512) == kRenderReplaced);
  callbacks.close(callbacks.user, via);
}

void TestMusicUsesBasePack() {
  std::printf("music pack: every stream plays with the base pack, never its stage pack's sound effects\n");
  MakeStreams();
  SoundSet set = MakeSet();
  const std::uint8_t song = g_streams[0].song_id;
  const std::uint16_t stage_pack = set.songs[song].pack_id;
  CHECK(stage_pack != kNoId && stage_pack != kMusicPack);
  set.packs[stage_pack].blocks[0].data.assign(set.packs[stage_pack].blocks[0].data.size(), 0x03);

  Pcm with_stage_pack(512 * 2);
  SoundtrackStream stage_stream;
  CHECK(stage_stream.Start(set, song, stage_pack, 44100) == SnesSoundtrackPlayer::StartResult::kOk);
  CHECK(stage_stream.Render(with_stage_pack.data(), 512) && Peak(with_stage_pack, 0) < 100);

  Pcm expected(512 * 2);
  SoundtrackStream base_stream;
  CHECK(base_stream.Start(set, song, kMusicPack, 44100) == SnesSoundtrackPlayer::StartResult::kOk);
  CHECK(base_stream.Render(expected.data(), 512) && Peak(expected, 0) > 1000);

  ClassicSoundtrackProvider provider(set, Warn, g_streams);
  for (size_t i = 0; i < g_streams.size(); ++i) {
    const std::int32_t id = OpenBuffer(provider, std::uint32_t(20 + i), i);
    Pcm supplied(512 * 2);
    CHECK(id >= 0 && provider.Render(id, supplied.data(), 512) == kRenderReplaced);
    CHECK(Peak(supplied, 0) > 1000);
    if (i == 0) {
      CHECK(supplied == expected);
    }
    provider.Close(id);
  }
}

void TestRealtimePath() {
  std::printf("audio path: no allocation in open, render and close after construction\n");
  MakeStreams();
  g_warnings.clear();
  ClassicSoundtrackProvider provider(MakeSet(), Warn, g_streams);
  Pcm frames(512 * 2);
  const std::int32_t warm = OpenBuffer(provider, 1, 0);
  provider.Render(warm, frames.data(), 512);
  provider.Close(warm);

  g_allocations = 0;
  g_count_allocations = true;
  bool rendered = true;
  for (int round = 0; round < 3; ++round) {
    const std::int32_t stream = OpenBuffer(provider, 1, size_t(round));
    for (int block = 0; block < 200; ++block) {
      rendered = rendered && provider.Render(stream, frames.data(), 512) == kRenderReplaced;
    }
    CHECK(OpenBuffer(provider, 1, size_t(round)) == stream);
    provider.Close(stream);
  }
  g_count_allocations = false;
  CHECK(rendered);
  CHECK(g_allocations.load() == 0);

  const fs::path root = TMNT_ROOT_DIR;
  for (const char* file : {"spc_player.cpp", "resampler.cpp", "soundtrack_stream.cpp", "music_streams.cpp", "xma_provider.cpp"}) {
    const std::string text = ReadText(root / "src" / "classic_soundtrack" / file);
    CHECK(!text.empty());
    for (const char* banned : {"fstream", "<filesystem>", "fopen", "ifstream", "toml", "Sha256", "LzDecompress", ".sfc", ".smc"}) {
      CHECK(!Has(text, banned));
    }
  }
  const std::string runtime = ReadText(root / "src" / "classic_soundtrack_runtime.cpp");
  const std::string start = ReadText(root / "src" / "classic_soundtrack" / "runtime_start.cpp");
  const std::string activation = ReadText(root / "src" / "classic_soundtrack" / "activation.cpp");
  CHECK(Has(runtime, "REXCVAR_GET(tmnt_classic_soundtrack), user_data_root,"));
  CHECK(Has(activation, "PrepareRuntimeStart(true, user_data_root)"));
  CHECK(Has(start, "QuerySoundtrackStatus(user_data_root, &result.set)"));
  for (const std::string* text : {&runtime, &start, &activation}) {
    for (const char* banned : {"ImportSoundtrack", "AnalyzeRom", "ReadRomFile", ".sfc", ".smc", ".swc", ".fig", "game/snes"}) {
      CHECK(!Has(*text, banned));
    }
  }
}

void TestNoRomAtPlayback() {
  std::printf("playback from soundset.bin alone, no ROM anywhere\n");
  const fs::path root = fs::temp_directory_path() / "tmnt_classic_playback_no_rom";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(SoundtrackFolder(root));
  const SoundSet set = MakeSet();
  const Bytes bytes = SerializeSoundSet(set);
  ClassicSoundtrackManifest manifest;
  manifest.format_version = kSoundSetFormatVersion;
  manifest.importer_version = kImporterVersion;
  manifest.source_rom_sha256 = std::string(64, 'a');
  manifest.source_region = "USA";
  manifest.soundtrack_fingerprint = SoundtrackFingerprint(set);
  manifest.soundset_sha256 = trg::update::Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  manifest.song_count = kSongCount;
  {
    std::ofstream(SoundtrackFolder(root) / kSoundSetFile, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    std::ofstream(SoundtrackFolder(root) / kManifestFile, std::ios::binary) << SerializeManifest(manifest);
  }
  SoundSet loaded;
  const StatusResult status = QuerySoundtrackStatus(root, &loaded);
  CHECK(status.status == SoundtrackStatus::kReady);
  CHECK(loaded.songs.size() == kSongCount);
  MakeStreams();
  ClassicSoundtrackProvider provider(std::move(loaded), Warn, g_streams);
  const std::int32_t stream = OpenBuffer(provider, 1, 0);
  Pcm frames(512 * 2);
  CHECK(stream >= 0 && provider.Render(stream, frames.data(), 512) == kRenderReplaced && Peak(frames, 0) > 0);

  ClassicSoundtrackManifest old = manifest;
  old.importer_version = 1;
  std::ofstream(SoundtrackFolder(root) / kManifestFile, std::ios::binary | std::ios::trunc) << SerializeManifest(old);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kImportFailed);
  fs::remove(SoundtrackFolder(root) / kSoundSetFile);
  SoundSet none;
  CHECK(QuerySoundtrackStatus(root, &none).status == SoundtrackStatus::kImportFailed && none.songs.empty());
  fs::remove_all(root, ec);
  CHECK(QuerySoundtrackStatus(root).status == SoundtrackStatus::kNotImported);
}

void TestWiring() {
  std::printf("wiring: setting Off by default, startup call, patch 0087, packaging, licenses\n");
  const fs::path root = TMNT_ROOT_DIR;
  const std::string settings = ReadText(root / "src" / "settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_classic_soundtrack, false,"));
  CHECK(Has(settings, "\"tmnt_classic_soundtrack\","));
  CHECK(Has(ReadText(root / "src" / "tmnt_turtles_in_time_reshelled_app.h"), "tmnt::StartClassicSoundtrack(user_data_root());"));
  CHECK(!Has(ReadText(root / "src" / "launcher.cpp"), "tmnt_classic_soundtrack"));
  CHECK(Has(ReadText(root / "docs" / "launcher.md"), "`tmnt_classic_soundtrack`"));

  const std::string patch = ReadText(root / "patches" / "rexglue-sdk" / "0087-xma-pcm-provider.patch");
  CHECK(Has(patch, "rex_audio_set_xma_pcm_provider") && Has(patch, "src/audio/xma_pcm_override.h"));
  for (const char* game_specific : {"TMNT", "SNES", "soundset", "snes_spc", "Konami"}) {
    CHECK(!Has(patch, game_specific));
  }
  CHECK(Has(ReadText(root / "patches" / "rexglue-sdk" / "patches.txt"), "0087-xma-pcm-provider.patch"));
  const std::string provider_header = ReadText(root / "src" / "classic_soundtrack" / "xma_provider.h");
  for (const char* field : {"int32_t (*open)(void* user, uint32_t context_id, const uint8_t* input, uint32_t input_bytes, uint32_t sample_rate,",
                            "int32_t (*render)(void* user, int32_t stream, int16_t* frames, uint32_t frame_count);",
                            "void (*close)(void* user, int32_t stream);"}) {
    CHECK(Has(provider_header, field) && Has(patch, field));
  }

  const std::string licenses = ReadText(root / "packaging" / "licenses" / "THIRD-PARTY-LICENSES.txt");
  CHECK(Has(licenses, "snes_spc (SPC700 and S-DSP emulator from Game_Music_Emu)"));
  CHECK(Has(licenses, "GNU LGPL 2.1 or later"));
  CHECK(Has(ReadText(root / "third_party" / "snes_spc" / "license.txt"), "GNU LESSER GENERAL PUBLIC LICENSE"));
  CHECK(Has(ReadText(root / "third_party" / "snes_spc" / "README.md"), "8ebb4f65096fdf980b39be2283699d523ddc09af"));
  const std::string cmake = ReadText(root / "CMakeLists.txt");
  CHECK(Has(cmake, "add_library(tmnt_snes_spc STATIC") && Has(cmake, "BLARGG_LITTLE_ENDIAN=1"));
  CHECK(Has(cmake, "trg::launcher tmnt_snes_spc"));
  for (const char* script : {"package-release.ps1", "linux-policy.sh"}) {
    const std::string text = ReadText(root / "tools" / script);
    for (const char* needle : {"*.wav", "*.ogg", "*.flac", "*.spc", "*.sfc", "snes_soundtrack", "soundset.bin"}) {
      CHECK(Has(text, needle));
    }
  }
  const std::string gitignore = ReadText(root / ".gitignore");
  CHECK(Has(gitignore, "*.wav") && Has(gitignore, "*.ogg") && Has(gitignore, "*.flac"));
}

}  // namespace

int main() {
  TestPlayer();
  TestPlaybackCheck();
  TestResampler();
  TestMusicStreams();
  TestProvider();
  TestNativeRate();
  TestResamplerTones();
  TestCallbackBoundaries();
  TestStereoFramesAndDrift();
  TestRuntimeSwitching();
  TestMusicUsesBasePack();
  TestRealtimePath();
  TestNoRomAtPlayback();
  TestWiring();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
