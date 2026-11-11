#include "classic_soundtrack/spc_player.h"

#include <algorithm>
#include <cstring>
#include <span>

#include "Snes_Spc.h"

namespace tmnt::classic {
namespace {

constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data v0.30\x1A\x1A";
constexpr size_t kSignatureBytes = 35;
constexpr size_t kVersionAt = 0x24;
constexpr size_t kPcLowAt = 0x25;
constexpr size_t kPcHighAt = 0x26;
constexpr size_t kStackAt = 0x2B;
constexpr size_t kRamAt = 0x100;
constexpr size_t kDspAt = 0x10100;
constexpr size_t kSmpRegisters = 0xF0;
constexpr size_t kDspFlagRegister = 0x6C;
// Power-on FLG: soft reset, mute and echo writes off until the driver programs the DSP.
constexpr std::uint8_t kPowerOnFlags = 0xE0;
constexpr std::uint8_t kStackStart = 0xEF;
constexpr size_t kBootFrames = SnesSoundtrackPlayer::kSampleRate * 60 / 1000;
constexpr size_t kAcknowledgeFrames = SnesSoundtrackPlayer::kSampleRate * 300 / 1000;
constexpr size_t kPollFrames = 64;

}  // namespace

SnesSoundtrackPlayer::SnesSoundtrackPlayer()
    : spc_(std::make_unique<Snes_Spc>()), image_(Snes_Spc::spc_file_size, 0) {
  spc_->init();
}

SnesSoundtrackPlayer::~SnesSoundtrackPlayer() = default;

SnesSoundtrackPlayer::StartResult SnesSoundtrackPlayer::Start(const SoundSet& set, size_t song_id, size_t pack_id) {
  playing_ = false;
  error_ = nullptr;
  if (song_id >= set.songs.size()) {
    return StartResult::kBadSong;
  }
  if (pack_id >= set.packs.size()) {
    return StartResult::kBadPack;
  }

  std::fill(image_.begin(), image_.end(), static_cast<unsigned char>(0));
  std::memcpy(image_.data(), kSpcSignature, kSignatureBytes);
  image_[kVersionAt] = 30;
  image_[kPcLowAt] = std::uint8_t(kDriverAddress);
  image_[kPcHighAt] = std::uint8_t(kDriverAddress >> 8);
  image_[kStackAt] = kStackStart;
  const std::span<std::uint8_t> ram(image_.data() + kRamAt, kAramSize);
  if (!WriteAramImage(set, int(pack_id), int(song_id), ram)) {
    return StartResult::kEmulatorError;
  }
  std::fill(ram.begin() + kSmpRegisters, ram.begin() + kSmpRegisters + 16, std::uint8_t(0));
  image_[kDspAt + kDspFlagRegister] = kPowerOnFlags;

  if (spc_->load_spc(image_.data(), long(image_.size())) != nullptr || !RunFrames(kBootFrames)) {
    return StartResult::kEmulatorError;
  }
  if (!SendCommand(set.songs[song_id].command)) {
    return StartResult::kNoAcknowledge;
  }
  playing_ = true;
  return StartResult::kOk;
}

void SnesSoundtrackPlayer::Stop() {
  playing_ = false;
}

size_t SnesSoundtrackPlayer::Render(std::int16_t* frames, size_t frame_count) {
  if (!playing_ || frame_count == 0) {
    return 0;
  }
  if (const char* error = spc_->play(int(frame_count * 2), frames)) {
    error_ = error;
    playing_ = false;
    return 0;
  }
  return frame_count;
}

bool SnesSoundtrackPlayer::RunFrames(size_t frame_count) {
  const size_t chunk = scratch_.size() / 2;
  for (size_t done = 0; done < frame_count; done += chunk) {
    const size_t frames = std::min(chunk, frame_count - done);
    if (const char* error = spc_->play(int(frames * 2), scratch_.data())) {
      error_ = error;
      return false;
    }
  }
  return true;
}

// The game's handshake: write the command to port 0 and wait for the driver to echo it.
bool SnesSoundtrackPlayer::SendCommand(std::uint8_t command) {
  spc_->write_port(0, 0, command);
  for (size_t waited = 0; waited < kAcknowledgeFrames; waited += kPollFrames) {
    if (!RunFrames(kPollFrames)) {
      return false;
    }
    if (spc_->read_port(0, 0) == command) {
      return true;
    }
  }
  return false;
}

const char* StartResultText(SnesSoundtrackPlayer::StartResult result) {
  switch (result) {
    case SnesSoundtrackPlayer::StartResult::kOk: return "started";
    case SnesSoundtrackPlayer::StartResult::kBadSong: return "no such song";
    case SnesSoundtrackPlayer::StartResult::kBadPack: return "no such sample pack";
    case SnesSoundtrackPlayer::StartResult::kEmulatorError: return "the SPC emulator stopped";
    case SnesSoundtrackPlayer::StartResult::kNoAcknowledge: return "the sound driver did not take the command";
  }
  return "unknown";
}

}  // namespace tmnt::classic
