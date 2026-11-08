#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include "classic_soundtrack/music_streams.h"
#include "classic_soundtrack/soundset.h"
#include "classic_soundtrack/soundtrack_stream.h"

// Same layout as the struct the patched runtime declares (patch 0087); the exe compiles against stock SDK headers.
extern "C" {
struct RexXmaPcmProvider {
  void* user;
  int32_t (*open)(void* user, uint32_t context_id, const uint8_t* input, uint32_t input_bytes, uint32_t sample_rate,
                  uint32_t channels);
  int32_t (*render)(void* user, int32_t stream, int16_t* frames, uint32_t frame_count);
  void (*close)(void* user, int32_t stream);
};
}

namespace tmnt::classic {

inline constexpr std::int32_t kRenderFailed = 0;
inline constexpr std::int32_t kRenderReplaced = 1;
inline constexpr std::int32_t kRenderPassThrough = 2;

// Replaces the Re-Shelled music streams listed in kMusicStreams with SNES songs while replacing() is on. A recognised
// stream keeps its slot either way, so switching back on restarts its song at once. Everything is allocated up
// front; open, render and close only look up tables and run the emulator.
class ClassicSoundtrackProvider {
 public:
  static constexpr size_t kStreamSlots = 6;
  static constexpr std::int32_t kNoStream = -1;
  using Warn = void (*)(const char* message);
  // DEBUG_SAULO: a large stream that is not a known music wave is logged once, up to this many distinct ones.
  static constexpr std::uint32_t kMinUnmatchedBytes = 32768;
  static constexpr size_t kUnmatchedLogLimit = 8;

  // note, when set, hears one line per stream that starts playing an SNES song.
  ClassicSoundtrackProvider(SoundSet set, Warn warn, std::span<const MusicStream> streams = kMusicStreams,
                            Warn note = nullptr);

  std::int32_t Open(std::uint32_t context_id, const std::uint8_t* input, std::uint32_t input_bytes,
                    std::uint32_t sample_rate, std::uint32_t channels);
  std::int32_t Render(std::int32_t stream, std::int16_t* frames, std::uint32_t frame_count);
  void Close(std::int32_t stream);

  RexXmaPcmProvider Callbacks();

  void SetReplacing(bool on) { replacing_.store(on, std::memory_order_release); }
  bool replacing() const { return replacing_.load(std::memory_order_acquire); }

  size_t active_streams() const;
  size_t playing_streams() const;
  bool song_failed(size_t song_id) const { return song_id < kSongCount && failed_[song_id].load(); }

 private:
  struct Slot {
    SoundtrackStream stream;
    bool used = false;
    bool started = false;
    std::uint32_t context = 0;
    std::uint32_t sample_rate = 0;
    const MusicStream* music = nullptr;
  };

  void Fail(size_t song_id, const char* what);
  void NoteUnmatched(std::uint64_t fingerprint, std::uint32_t input_bytes, std::uint32_t channels);
  bool StartSong(Slot& slot);

  SoundSet set_;
  Warn warn_;
  Warn note_;
  std::span<const MusicStream> streams_;
  std::atomic<bool> replacing_{true};
  // Guards slot ownership. Render runs without it except to start a song: the runtime calls one context's callbacks
  // under that context's lock, and a slot belongs to exactly one context between open and close.
  mutable std::mutex mutex_;
  std::array<std::unique_ptr<Slot>, kStreamSlots> slots_;
  std::array<std::atomic<bool>, kSongCount> failed_{};
  std::array<std::uint64_t, kUnmatchedLogLimit> unmatched_{};
  size_t unmatched_count_ = 0;
};

}  // namespace tmnt::classic
