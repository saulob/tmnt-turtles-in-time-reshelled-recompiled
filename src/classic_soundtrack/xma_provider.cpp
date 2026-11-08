#include "classic_soundtrack/xma_provider.h"

#include <cstdio>
#include <utility>

#include "classic_soundtrack/known_roms.h"

namespace tmnt::classic {
namespace {

constexpr std::uint32_t kStereo = 2;

}  // namespace

ClassicSoundtrackProvider::ClassicSoundtrackProvider(SoundSet set, Warn warn, std::span<const MusicStream> streams,
                                                     Warn note)
    : set_(std::move(set)), warn_(warn), note_(note), streams_(streams) {
  for (auto& slot : slots_) {
    slot = std::make_unique<Slot>();
  }
}

size_t ClassicSoundtrackProvider::active_streams() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& slot : slots_) {
    count += slot->used ? 1 : 0;
  }
  return count;
}

size_t ClassicSoundtrackProvider::playing_streams() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& slot : slots_) {
    count += slot->started ? 1 : 0;
  }
  return count;
}

void ClassicSoundtrackProvider::Fail(size_t song_id, const char* what) {
  if (song_id < kSongCount && !failed_[song_id].exchange(true) && warn_) {
    const std::string_view label = SongLabel(song_id).empty() ? std::string_view("an unnamed song") : SongLabel(song_id);
    char message[160];
    std::snprintf(message, sizeof(message), "Classic soundtrack: %s for %.*s; the original music plays instead", what,
                  int(label.size()), label.data());
    warn_(message);
  }
}

void ClassicSoundtrackProvider::NoteUnmatched(std::uint64_t fingerprint, std::uint32_t input_bytes,
                                              std::uint32_t channels) {
  if (!note_ || input_bytes < kMinUnmatchedBytes) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < unmatched_count_; ++i) {
    if (unmatched_[i] == fingerprint) {
      return;
    }
  }
  if (unmatched_count_ == unmatched_.size()) {
    return;
  }
  unmatched_[unmatched_count_++] = fingerprint;
  char message[160];
  std::snprintf(message, sizeof(message),
                "Classic soundtrack: DEBUG_SAULO XMA stream not recognised, %u bytes, %u channels, fingerprint %016llX",
                unsigned(input_bytes), unsigned(channels), static_cast<unsigned long long>(fingerprint));
  note_(message);
}

// Booting the emulated driver takes about 60 ms of emulated time. The caller holds mutex_.
bool ClassicSoundtrackProvider::StartSong(Slot& slot) {
  const MusicStream& music = *slot.music;
  const size_t pack = kMusicPack;
  const auto result = slot.stream.Start(set_, music.song_id, pack, slot.sample_rate);
  if (result != SnesSoundtrackPlayer::StartResult::kOk) {
    slot.stream.Stop();
    Fail(music.song_id, StartResultText(result));
    return false;
  }
  slot.started = true;
  if (note_) {
    const std::string_view label = SongLabel(music.song_id);
    char message[192];
    std::snprintf(message, sizeof(message), "Classic soundtrack: %.*s plays SNES song %u %.*s with pack %zu at %u Hz",
                  int(music.context.size()), music.context.data(), unsigned(music.song_id), int(label.size()),
                  label.data(), pack, unsigned(slot.sample_rate));
    note_(message);
  }
  return true;
}

std::int32_t ClassicSoundtrackProvider::Open(std::uint32_t context_id, const std::uint8_t* input,
                                             std::uint32_t input_bytes, std::uint32_t sample_rate,
                                             std::uint32_t channels) {
  if (!input || sample_rate == 0) {
    return kNoStream;
  }
  const std::uint64_t fingerprint = XmaStreamFingerprint(input, input_bytes);
  const MusicStream* music = FindMusicStream(streams_, fingerprint, input_bytes);
  if (!music || channels != kStereo) {
    NoteUnmatched(fingerprint, input_bytes, channels);
    return kNoStream;
  }
  if (music->song_id >= set_.songs.size() || failed_[music->song_id].load()) {
    return kNoStream;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& slot = *slots_[i];
    if (slot.used && slot.context == context_id && slot.music == music) {
      return std::int32_t(i);
    }
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    Slot& slot = *slots_[i];
    if (slot.used) {
      continue;
    }
    slot.context = context_id;
    slot.sample_rate = sample_rate;
    slot.music = music;
    if (replacing() && !StartSong(slot)) {
      slot.music = nullptr;
      return kNoStream;
    }
    slot.used = true;
    return std::int32_t(i);
  }
  if (warn_) {
    warn_("Classic soundtrack: every stream is busy; the original music plays instead");
  }
  return kNoStream;
}

std::int32_t ClassicSoundtrackProvider::Render(std::int32_t stream, std::int16_t* frames, std::uint32_t frame_count) {
  if (stream < 0 || size_t(stream) >= slots_.size() || !frames) {
    return kRenderFailed;
  }
  Slot& slot = *slots_[size_t(stream)];
  if (!slot.used) {
    return kRenderFailed;
  }
  if (!replacing()) {
    if (slot.started) {
      slot.stream.Stop();
      slot.started = false;
    }
    return kRenderPassThrough;
  }
  if (!slot.started) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!StartSong(slot)) {
      return kRenderFailed;
    }
  }
  if (!slot.stream.Render(frames, frame_count)) {
    Fail(slot.music->song_id, "the SPC emulator stopped");
    return kRenderFailed;
  }
  return kRenderReplaced;
}

void ClassicSoundtrackProvider::Close(std::int32_t stream) {
  if (stream < 0 || size_t(stream) >= slots_.size()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot& slot = *slots_[size_t(stream)];
  slot.stream.Stop();
  slot.used = false;
  slot.started = false;
  slot.music = nullptr;
}

RexXmaPcmProvider ClassicSoundtrackProvider::Callbacks() {
  RexXmaPcmProvider callbacks{};
  callbacks.user = this;
  callbacks.open = [](void* user, uint32_t context, const uint8_t* input, uint32_t bytes, uint32_t rate,
                      uint32_t channels) {
    return static_cast<ClassicSoundtrackProvider*>(user)->Open(context, input, bytes, rate, channels);
  };
  callbacks.render = [](void* user, int32_t stream, int16_t* frames, uint32_t count) {
    return static_cast<ClassicSoundtrackProvider*>(user)->Render(stream, frames, count);
  };
  callbacks.close = [](void* user, int32_t stream) { static_cast<ClassicSoundtrackProvider*>(user)->Close(stream); };
  return callbacks;
}

}  // namespace tmnt::classic
