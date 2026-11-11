#include "classic_soundtrack/resampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tmnt::classic {

bool StereoResampler::Configure(std::uint32_t input_rate, std::uint32_t output_rate) {
  if (input_rate == 0 || output_rate == 0) {
    return false;
  }
  const std::uint32_t divisor = std::gcd(input_rate, output_rate);
  input_step_ = input_rate / divisor;
  output_step_ = output_rate / divisor;
  Reset();
  return true;
}

void StereoResampler::Reset() {
  phase_ = 0;
  consumed_ = 0;
  history_.fill(0.0f);
  chunk_at_ = 0;
  chunk_frames_ = 0;
}

void StereoResampler::Push(std::int16_t left, std::int16_t right) {
  std::copy(history_.begin() + 2, history_.end(), history_.begin());
  history_[6] = left;
  history_[7] = right;
  ++consumed_;
}

// Catmull-Rom between the middle two of the last four input frames.
void StereoResampler::Interpolate(std::int16_t* out) const {
  const float t = float(phase_) / float(output_step_);
  for (int channel = 0; channel < 2; ++channel) {
    const float x0 = history_[0 + channel];
    const float x1 = history_[2 + channel];
    const float x2 = history_[4 + channel];
    const float x3 = history_[6 + channel];
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    const float value = ((c3 * t + c2) * t + c1) * t + x1;
    out[channel] = std::int16_t(std::clamp(std::lround(value), -32768L, 32767L));
  }
}

}  // namespace tmnt::classic
