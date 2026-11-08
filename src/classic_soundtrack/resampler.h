#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tmnt::classic {

// Stereo int16 rate conversion with an exact rational phase, so output and input counts never drift apart.
class StereoResampler {
 public:
  static constexpr size_t kChunkFrames = 256;

  bool Configure(std::uint32_t input_rate, std::uint32_t output_rate);
  void Reset();

  // Writes exactly frame_count frames. pull(int16_t* dst, size_t frames) must return the frames it wrote; a short
  // pull is padded with silence and reported by the return value.
  template <class Pull>
  bool Process(std::int16_t* out, size_t frame_count, Pull&& pull);

  std::uint32_t input_step() const { return input_step_; }
  std::uint32_t output_step() const { return output_step_; }
  std::uint64_t consumed_frames() const { return consumed_; }

 private:
  void Push(std::int16_t left, std::int16_t right);
  void Interpolate(std::int16_t* out) const;

  std::uint32_t input_step_ = 1;
  std::uint32_t output_step_ = 1;
  std::uint32_t phase_ = 0;
  std::uint64_t consumed_ = 0;
  std::array<float, 8> history_{};
  std::array<std::int16_t, kChunkFrames * 2> chunk_{};
  size_t chunk_at_ = 0;
  size_t chunk_frames_ = 0;
};

template <class Pull>
bool StereoResampler::Process(std::int16_t* out, size_t frame_count, Pull&& pull) {
  bool complete = true;
  for (size_t i = 0; i < frame_count; ++i) {
    Interpolate(out + i * 2);
    phase_ += input_step_;
    while (phase_ >= output_step_) {
      phase_ -= output_step_;
      if (chunk_at_ == chunk_frames_) {
        chunk_frames_ = pull(chunk_.data(), kChunkFrames);
        chunk_at_ = 0;
        if (chunk_frames_ < kChunkFrames) {
          complete = false;
          for (size_t f = chunk_frames_; f < kChunkFrames; ++f) {
            chunk_[f * 2] = chunk_[f * 2 + 1] = 0;
          }
          chunk_frames_ = kChunkFrames;
        }
      }
      Push(chunk_[chunk_at_ * 2], chunk_[chunk_at_ * 2 + 1]);
      ++chunk_at_;
    }
  }
  return complete;
}

}  // namespace tmnt::classic
