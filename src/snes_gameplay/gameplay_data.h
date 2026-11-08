#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "snes_gameplay/rules.h"

namespace tmnt::snes {

inline constexpr uint32_t kGameplayFormatVersion = 1;
inline constexpr uint32_t kGameplayExtractorVersion = 1;
inline constexpr char kGameplayFile[] = "snes_gameplay.bin";

inline constexpr double kNtscFrameRate = 60.0988;
inline constexpr double kPalFrameRate = 50.0070;

enum class VideoStandard : uint8_t { kNtsc = 0, kPal = 1 };

// Turtle order of the SNES character select: Leonardo, Michelangelo, Donatello, Raphael.
inline constexpr size_t kTurtles = 4;

// Compact parameters derived from the player's own ROM. Speeds are SNES pixels per frame in 16.16 fixed point.
struct GameplayData {
  VideoStandard video = VideoStandard::kNtsc;
  std::array<Facing, 8> facing{};
  uint8_t lane_tolerance = 0;
  uint8_t grab_range = 0;
  uint8_t attack_search_range = 0;
  std::array<uint8_t, kTurtles> back_attack_range{};
  uint8_t throw_hold_frames = 0;
  uint8_t dash_hold_frames = 0;
  uint8_t dash_tap_frames = 0;
  bool dash_auto_default = false;
  int32_t run_speed = 0;
  std::array<int32_t, kTurtles> walk_speed{};
  int32_t dash_attack_speed = 0;
  uint8_t dash_attack_frames = 0;

  bool operator==(const GameplayData&) const = default;
};

double FrameRate(VideoStandard video);
RuntimeData ToRuntimeData(const GameplayData& data);

struct ExtractResult {
  bool ok = false;
  std::string detail;
  GameplayData data;
};

// rom is a normalized 1 MiB LoROM image. Every value is read only where its code matches exactly once.
ExtractResult ExtractGameplay(std::span<const uint8_t> rom);

std::vector<uint8_t> SerializeGameplay(const GameplayData& data, std::string_view source_rom_sha256);

enum class GameplayFileError { kOk, kMalformed, kOtherVersion, kChecksum };

GameplayFileError ParseGameplay(std::span<const uint8_t> bytes, GameplayData& data, std::string& source_rom_sha256);

enum class GameplayStatus { kNotImported, kReady, kReimportRequired, kUnsupportedRom };

struct GameplayStatusResult {
  GameplayStatus status = GameplayStatus::kNotImported;
  std::string region;
  std::string detail;
  GameplayData data;
};

// The gameplay data is part of the Classic Soundtrack import: it is usable only next to a Ready import of the same ROM.
GameplayStatusResult QueryGameplayStatus(const std::filesystem::path& user_data_root);

// Launcher text, in English: the launcher translates it.
const char* GameplayStatusLabel(GameplayStatus status);
inline bool GameplayCanEnable(GameplayStatus status) { return status == GameplayStatus::kReady; }

}  // namespace tmnt::snes
