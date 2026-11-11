#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tmnt::classic {

// The S-DSP output peaks near -6 dBFS (RMS around -25 dBFS), so the SNES PCM gets a fixed lift before the game's Music
// volume, which then scales both soundtracks the same way.
inline constexpr float kSnesSoundtrackGain = 1.5f;  // about +3.5 dB
// Above the knee the lifted signal bends smoothly toward full scale instead of hard clipping.
inline constexpr float kSoftKnee = 24576.0f;

inline std::int16_t LiftSample(std::int16_t sample) {
  const float lifted = float(sample) * kSnesSoundtrackGain;
  const float magnitude = std::fabs(lifted);
  if (magnitude <= kSoftKnee) {
    return std::int16_t(std::lrintf(lifted));
  }
  const float room = 32767.0f - kSoftKnee;
  const float bent = kSoftKnee + room * std::tanh((magnitude - kSoftKnee) / room);
  return std::int16_t(std::lrintf(std::copysign(std::min(bent, 32767.0f), lifted)));
}

inline void LiftSnesSoundtrack(std::int16_t* samples, size_t count) {
  for (size_t i = 0; i < count; ++i) samples[i] = LiftSample(samples[i]);
}

}  // namespace tmnt::classic
