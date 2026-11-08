#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace tmnt::snes {

inline constexpr char kDirectionalCombatSetting[] = "tmnt_snes_directional_combat";
inline constexpr char kThrowControlSetting[] = "tmnt_snes_throw_control";
inline constexpr char kDashSetting[] = "tmnt_snes_dash";
// Written by default.xex before the guest starts, read by TMNT_r: the two modules share only the cvar registry.
inline constexpr char kRuntimeDataSetting[] = "tmnt_snes_gameplay_data";

inline constexpr int kControllers = 4;
// Re-Shelled's Turtle_Defs.h values for m_LastFacing.
inline constexpr int kLeft = 0;
inline constexpr int kRight = 1;
inline constexpr int kNoDirection = -1;

enum class Facing : uint8_t { kKeep, kLeft, kRight };

// The values the native mechanics read at run time, all derived from the imported ROM.
struct RuntimeData {
  double frame_rate = 0.0;
  std::array<Facing, 8> facing{};
  uint32_t dash_hold_frames = 0;
  uint32_t throw_hold_frames = 0;

  double Seconds(uint32_t frames) const { return frame_rate > 0.0 ? frames / frame_rate : 0.0; }
};

inline char FacingChar(Facing facing) {
  return facing == Facing::kLeft ? 'L' : (facing == Facing::kRight ? 'R' : 'K');
}

inline std::string EncodeRuntimeData(const RuntimeData& data) {
  std::string facing;
  for (const Facing f : data.facing) {
    facing += FacingChar(f);
  }
  char rate[32];
  std::snprintf(rate, sizeof rate, "%.4f", data.frame_rate);
  return "v1 " + std::string(rate) + " " + facing + " " + std::to_string(data.dash_hold_frames) + " " +
         std::to_string(data.throw_hold_frames);
}

inline std::optional<RuntimeData> DecodeRuntimeData(std::string_view text) {
  const auto take = [&text]() {
    const size_t space = text.find(' ');
    const std::string_view word = text.substr(0, space);
    text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
    return word;
  };
  const auto number = [](std::string_view word, uint32_t& value) {
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    return error == std::errc() && end == word.data() + word.size();
  };
  if (take() != "v1") {
    return std::nullopt;
  }
  RuntimeData data;
  const std::string rate(take());
  char* rate_end = nullptr;
  data.frame_rate = std::strtod(rate.c_str(), &rate_end);
  if (rate.empty() || *rate_end != '\0' || !(data.frame_rate >= 40.0 && data.frame_rate <= 70.0)) {
    return std::nullopt;
  }
  const std::string_view facing = take();
  if (facing.size() != data.facing.size()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < facing.size(); ++i) {
    switch (facing[i]) {
      case 'K': data.facing[i] = Facing::kKeep; break;
      case 'L': data.facing[i] = Facing::kLeft; break;
      case 'R': data.facing[i] = Facing::kRight; break;
      default: return std::nullopt;
    }
  }
  if (!number(take(), data.dash_hold_frames) || !number(take(), data.throw_hold_frames) || !text.empty() ||
      data.dash_hold_frames == 0 || data.dash_hold_frames > 255 || data.throw_hold_frames > 255) {
    return std::nullopt;
  }
  return data;
}

// The SNES 8-way direction a stick vector reads as: 0 right, then clockwise on screen to 7 up-right. Re-Shelled's
// controller gives +x right and +y up. A centred stick has no direction.
inline int SnesDirection(float x, float y) {
  if (!(x * x + y * y > 1e-6f)) {
    return kNoDirection;
  }
  constexpr double kQuarterPi = 0.78539816339744830962;
  const int sector = static_cast<int>(std::floor(std::atan2(double(y), double(x)) / kQuarterPi + 0.5)) & 7;
  return (8 - sector) & 7;
}

inline bool IsHorizontal(int direction) { return direction == 0 || direction == 4; }

// m_LastFacing after a move: a direction without a horizontal part leaves the facing as it was.
inline int NextFacing(int current, int direction, const RuntimeData& data) {
  if (direction < 0 || direction > 7) {
    return current;
  }
  switch (data.facing[direction]) {
    case Facing::kLeft: return kLeft;
    case Facing::kRight: return kRight;
    case Facing::kKeep: break;
  }
  return current;
}

// One controller's stick as the SNES player code sees it.
struct StickState {
  int direction = kNoDirection;
  int previous = kNoDirection;
  double held = 0.0;      // since the stick left the centre
  double same_held = 0.0; // in the current direction
  bool dashing = false;
};

inline void ReleaseStick(StickState& stick) { stick = StickState{}; }

// What a feature keeps between frames, dropped when it is switched off so it never resumes from a stale count.
inline void ResetDashState(StickState& stick) {
  stick.same_held = 0.0;
  stick.dashing = false;
}

inline void ResetThrowState(StickState& stick) {
  stick.held = 0.0;
  stick.previous = kNoDirection;
}

// A run-strength push for dt seconds. The SNES Auto dash starts once one horizontal direction has been held for
// dash_hold_frames; any other direction walks and starts the count again.
inline bool PushStick(StickState& stick, int direction, double dt, const RuntimeData& data) {
  stick.previous = stick.direction;
  if (direction != stick.direction) {
    stick.direction = direction;
    stick.same_held = 0.0;
    stick.dashing = false;
  }
  stick.held += dt;
  stick.same_held += dt;
  if (!IsHorizontal(direction)) {
    stick.dashing = false;
    return false;
  }
  constexpr double kSlack = 1e-6;
  if (!stick.dashing && stick.same_held + kSlack >= data.Seconds(data.dash_hold_frames)) {
    stick.dashing = true;
  }
  return stick.dashing;
}

// A walk-strength push: the stick is held but the turtle walks.
inline void HoldStick(StickState& stick, int direction, double dt) {
  stick.previous = stick.direction;
  if (direction != stick.direction) {
    stick.direction = direction;
    stick.same_held = 0.0;
  }
  stick.dashing = false;
  stick.held += dt;
  stick.same_held += dt;
}

enum class GrabChoice { kSlam, kThrowToScreen };

// The SNES throw selector (80:B14E): only a straight horizontal push toward the facing side, held since the previous
// frame and for throw_hold_frames since the stick left the centre, throws; every other input slams.
inline GrabChoice ChooseGrab(const StickState& stick, int facing, const RuntimeData& data) {
  const int direction = stick.direction;
  if (!IsHorizontal(direction) || stick.previous != direction) {
    return GrabChoice::kSlam;
  }
  const Facing toward = data.facing[direction];
  if ((toward == Facing::kLeft ? kLeft : (toward == Facing::kRight ? kRight : kNoDirection)) != facing) {
    return GrabChoice::kSlam;
  }
  constexpr double kSlack = 1e-6;
  return stick.held + kSlack >= data.Seconds(data.throw_hold_frames) ? GrabChoice::kThrowToScreen : GrabChoice::kSlam;
}

}  // namespace tmnt::snes
