#include "snes_gameplay/gameplay_data.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>

#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/snes_rom.h"
#include "updater/sha256.h"

namespace tmnt::snes {
namespace {

namespace fs = std::filesystem;

// The player code's data bank.
constexpr uint8_t kDataBank = 0x81;
constexpr char kMagic[8] = {'T', 'M', 'N', 'T', 'S', 'N', 'E', 'S'};
constexpr size_t kShaHex = 64;
constexpr size_t kPayloadSize = 45;
constexpr size_t kMaxGameplayFile = 4096;

// "??" marks a value the extractor reads; every other byte must match.
struct Signature {
  const char* name;
  const char* bytes;
};

constexpr Signature kFacing{"facing", "A0 00 00 38 E9 ?? 00 89 ?? 00 F0 11 C9 ?? 00 B0 02 C8 C8 B5 04 29 DF DF 19 05 95 95 04 6B"};
constexpr Signature kDirection{"direction", "A9 00 00 E2 20 A5 9B 29 0F A8 B9 F5 94 C2 20 30 35"};
constexpr Signature kThrowWindow{"throw window", "B5 5C D5 32 F0 05 A9 ?? 00 95 6A B5 6A F0 02 D6 6A"};
constexpr Signature kThrowSelect{
    "throw selector",
    "A5 82 C9 03 00 D0 0B A5 8A C9 03 00 D0 04 A9 00 00 60 A5 08 0A 95 6C D0 1E A5 8C C9 04 00 F0 17 B5 6A D0 13 A5 9A "
    "EB 29 0F 00 89 ?? 00 D0 08 0A A8 B9 1B 96 D5 5C 60"};
constexpr Signature kScreenThrow{"screen throw", "AD D4 18 D0 DE A4 00 AD D2 18 C9 03 00 F0 10 C9 02 00 D0 CF"};
constexpr Signature kTargetSearch{
    "target search",
    "B5 04 29 20 20 F0 02 7B 1A 85 00 64 08 9B A2 D0 08 B5 14 C9 30 01 90 55 B5 10 D0 51 64 02 B5 08 38 F9 08 00 10 06 "
    "49 FF FF 1A E6 02 85 04 C9 ?? 00 B0 3A B5 0C 38 F9 0C 00 18 69 ?? 00 C9 ?? 00 B0 2B A5 02 C5 00 D0 33 A5 04 C9 ?? "
    "00 B0 1E B5 14 C9 60 01 90 17 C9 8D 01 B0 24 B5 16 C9 4E 00 90 0B C9 4F 00 B0 06"};
constexpr Signature kBackAttack{"back attack", "B4 14 A5 04 D9 4D 96 90 68"};
constexpr Signature kDashTap{"dash tap",
                             "AD E8 1F F0 1A B5 36 D0 10 B5 32 F0 05 C9 04 00 D0 0A 95 5E A9 ?? 00 95 42 F6 36 6B 74 36 "
                             "6B A9 ?? 00 95 34 6B"};
constexpr Signature kDashTrigger{"dash trigger",
                                 "B5 30 89 01 00 D0 2C AD E8 1F D0 28 B5 32 F0 05 C9 04 00 D0 1E D6 34 D0 1A A9 0C 00 95 16"};
constexpr Signature kOptionDefaults{"option defaults",
                                    "A9 00 40 8D E2 1F 8D C0 1F A9 00 80 8D E4 1F 8D C2 1F ?? ?? ?? ?? ?? ??"};
constexpr Signature kRunSpeed{"run speed", "B5 5C 4A A8 74 22 74 20 B9 D3 94 95 1E B9 D7 94 95 1C 6B"};
constexpr Signature kWalkSpeed{"walk speed",
                               "B4 14 A5 82 C9 02 00 F0 05 C9 07 00 D0 03 A0 08 00 B9 C1 94 85 10 74 1E 74 22 B5 5C 0A 0A "
                               "18 65 10 A8 B9 63 96 95 1D"};
constexpr Signature kDashAttack{"dash attack",
                                "A9 0D 00 22 74 C8 00 A9 00 00 22 16 C0 00 B4 14 B9 9B 95 22 6E 93 04 A9 ?? 00 95 34 6B"};
constexpr Signature kDashAttackSpeed{"dash attack speed",
                                     "0A 0A 85 18 B5 5C 18 69 02 00 29 04 00 4A 18 65 18 A8 B9 C7 96 95 1E B9 D7 96 95 1C 6B"};

struct Pattern {
  std::vector<int> bytes;  // -1 is a read value
};

Pattern ParsePattern(const char* text) {
  Pattern pattern;
  for (const char* p = text; *p;) {
    if (*p == ' ') {
      ++p;
      continue;
    }
    if (p[0] == '?' && p[1] == '?') {
      pattern.bytes.push_back(-1);
    } else {
      pattern.bytes.push_back(int(std::strtol(std::string(p, 2).c_str(), nullptr, 16)));
    }
    p += 2;
  }
  return pattern;
}

class Extractor {
 public:
  explicit Extractor(std::span<const uint8_t> rom) : rom_(rom), reader_(rom) {}

  // The values at the signature's "??" bytes, or nothing unless the code matches exactly once.
  std::optional<std::vector<uint8_t>> Match(const Signature& signature) {
    const Pattern pattern = ParsePattern(signature.bytes);
    std::optional<size_t> found;
    for (size_t at = 0; at + pattern.bytes.size() <= rom_.size(); ++at) {
      if (!MatchesAt(pattern, at)) {
        continue;
      }
      if (found) {
        return Fail(signature, "matches more than once");
      }
      found = at;
    }
    if (!found) {
      return Fail(signature, "was not found");
    }
    std::vector<uint8_t> values;
    for (size_t i = 0; i < pattern.bytes.size(); ++i) {
      if (pattern.bytes[i] < 0) {
        values.push_back(rom_[*found + i]);
      }
    }
    return values;
  }

  std::optional<int16_t> Word(uint16_t address) {
    uint16_t value = 0;
    if (!reader_.ReadU16(kDataBank, address, value)) {
      error_ = "a gameplay table is outside the ROM";
      return std::nullopt;
    }
    return int16_t(value);
  }

  std::optional<int8_t> Byte(uint16_t address) {
    uint8_t value = 0;
    if (!reader_.ReadU8(kDataBank, address, value)) {
      error_ = "a gameplay table is outside the ROM";
      return std::nullopt;
    }
    return int8_t(value);
  }

  std::nullopt_t Invalid(const std::string& what) {
    if (error_.empty()) {
      error_ = what;
    }
    return std::nullopt;
  }

  const std::string& error() const { return error_; }

 private:
  bool MatchesAt(const Pattern& pattern, size_t at) const {
    for (size_t i = 0; i < pattern.bytes.size(); ++i) {
      if (pattern.bytes[i] >= 0 && rom_[at + i] != pattern.bytes[i]) {
        return false;
      }
    }
    return true;
  }

  std::nullopt_t Fail(const Signature& signature, const char* why) {
    if (error_.empty()) {
      error_ = std::string("the ") + signature.name + " code " + why;
    }
    return std::nullopt;
  }

  std::span<const uint8_t> rom_;
  classic::RomReader reader_;
  std::string error_;
};

int32_t Fixed16(int16_t whole, int16_t fraction) {
  return int32_t(uint32_t(uint16_t(whole)) << 16 | uint16_t(fraction));
}

// 80:B370: d - sub, masked; zero keeps the facing, below split loads the flipped (left) flags at 81:9507.
std::optional<std::array<Facing, 8>> DeriveFacing(Extractor& x, const std::vector<uint8_t>& code) {
  const auto right_flags = x.Word(0x9505);
  const auto left_flags = x.Word(0x9507);
  if (!right_flags || !left_flags) {
    return std::nullopt;
  }
  constexpr uint16_t kFlip = 0x2020;
  if ((uint16_t(*right_flags) & kFlip) != 0 || (uint16_t(*left_flags) & kFlip) != kFlip) {
    return x.Invalid("the facing flags are not the known ones");
  }
  const uint16_t sub = code[0];
  const uint16_t mask = code[1];
  const uint16_t split = code[2];
  std::array<Facing, 8> facing{};
  for (uint16_t d = 0; d < 8; ++d) {
    const uint16_t t = uint16_t(d - sub);
    if ((t & mask) == 0) {
      facing[d] = Facing::kKeep;
    } else {
      facing[d] = t < split ? Facing::kLeft : Facing::kRight;
    }
  }
  if (facing[0] != Facing::kRight || facing[4] != Facing::kLeft) {
    return x.Invalid("the facing rule is not the known one");
  }
  return facing;
}

// 81:94F5 turns the pad's up/down/left/right bits into the eight directions Re-Shelled's stick is read as.
bool DirectionTableKnown(Extractor& x) {
  constexpr std::array<std::pair<uint8_t, int8_t>, 8> kPad{
      {{0x1, 0}, {0x5, 1}, {0x4, 2}, {0x6, 3}, {0x2, 4}, {0xA, 5}, {0x8, 6}, {0x9, 7}}};
  for (const auto& [bits, direction] : kPad) {
    const auto value = x.Byte(uint16_t(0x94F5 + bits));
    if (!value || *value != direction) {
      return false;
    }
  }
  const auto none = x.Byte(0x94F5);
  return none && *none < 0;
}

// 81:961B maps the pad's left/right bits to a direction: nothing, right, left, both.
bool ThrowTableKnown(Extractor& x) {
  constexpr std::array<int16_t, 4> kExpected{-1, 0, 4, 0};
  for (size_t i = 0; i < kExpected.size(); ++i) {
    const auto value = x.Word(uint16_t(0x961B + 2 * i));
    if (!value || *value != kExpected[i]) {
      return false;
    }
  }
  return true;
}

std::optional<VideoStandard> VideoOf(std::span<const uint8_t> rom) {
  classic::RomHeaderInfo header;
  if (classic::ValidateLoRomHeader(rom, &header) != classic::RomStatus::kOk) {
    return std::nullopt;
  }
  switch (header.region) {
    case 0x00: case 0x01: case 0x0D: case 0x0F: case 0x10:
      return VideoStandard::kNtsc;
    default:
      return header.region <= 0x11 ? std::optional(VideoStandard::kPal) : std::nullopt;
  }
}

std::string AsText(std::span<const uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

uint32_t GetU32(std::span<const uint8_t> in, size_t at) {
  return uint32_t(in[at]) | uint32_t(in[at + 1]) << 8 | uint32_t(in[at + 2]) << 16 | uint32_t(in[at + 3]) << 24;
}

bool IsShaHex(std::string_view text) {
  return text.size() == kShaHex &&
         std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

double FrameRate(VideoStandard video) {
  return video == VideoStandard::kPal ? kPalFrameRate : kNtscFrameRate;
}

RuntimeData ToRuntimeData(const GameplayData& data) {
  RuntimeData runtime;
  runtime.frame_rate = FrameRate(data.video);
  runtime.facing = data.facing;
  runtime.dash_hold_frames = data.dash_hold_frames;
  runtime.throw_hold_frames = data.throw_hold_frames;
  return runtime;
}

ExtractResult ExtractGameplay(std::span<const uint8_t> rom) {
  ExtractResult result;
  Extractor x(rom);
  GameplayData& data = result.data;
  const auto fail = [&]() {
    result.detail = x.error().empty() ? "the gameplay code is not a known one" : x.error();
    return result;
  };

  const auto video = VideoOf(rom);
  if (!video) {
    result.detail = "the ROM header is not a known region";
    return result;
  }
  data.video = *video;

  const auto facing_code = x.Match(kFacing);
  if (!facing_code || !x.Match(kDirection)) return fail();
  const auto facing = DeriveFacing(x, *facing_code);
  if (!facing || !DirectionTableKnown(x)) return fail();
  data.facing = *facing;

  const auto search = x.Match(kTargetSearch);
  if (!search) return fail();
  data.attack_search_range = (*search)[0];
  data.lane_tolerance = (*search)[1];
  data.grab_range = (*search)[3];
  if (data.lane_tolerance == 0 || (*search)[2] != 2 * data.lane_tolerance || data.grab_range == 0 ||
      data.grab_range > data.attack_search_range) {
    x.Invalid("the attack lane is not the known one");
    return fail();
  }
  if (!x.Match(kBackAttack)) return fail();
  for (size_t t = 0; t < kTurtles; ++t) {
    const auto range = x.Word(uint16_t(0x964D + 2 * (t + 1)));
    if (!range || *range <= 0 || *range > 255) return fail();
    data.back_attack_range[t] = uint8_t(*range);
  }

  const auto window = x.Match(kThrowWindow);
  const auto select = x.Match(kThrowSelect);
  if (!window || !select || !x.Match(kScreenThrow)) return fail();
  if ((*select)[0] != 0x0C || !ThrowTableKnown(x)) {
    x.Invalid("the throw selector is not the known one");
    return fail();
  }
  data.throw_hold_frames = (*window)[0];

  const auto tap = x.Match(kDashTap);
  if (!tap || !x.Match(kDashTrigger)) return fail();
  data.dash_tap_frames = (*tap)[0];
  data.dash_hold_frames = (*tap)[1];
  if (data.dash_hold_frames == 0 || data.dash_tap_frames == 0) {
    x.Invalid("the dash timing is not the known one");
    return fail();
  }
  const auto defaults = x.Match(kOptionDefaults);
  if (!defaults) return fail();
  const std::vector<uint8_t> auto_dash{0x9C, 0xE8, 0x1F, 0x9C, 0xC6, 0x1F};
  const std::vector<uint8_t> manual_dash{0xA9, 0x01, 0x00, 0x8D, 0xE8, 0x1F};
  if (*defaults != auto_dash && *defaults != manual_dash) {
    x.Invalid("the dash option default is not a known one");
    return fail();
  }
  data.dash_auto_default = *defaults == auto_dash;

  if (!x.Match(kRunSpeed) || !x.Match(kWalkSpeed) || !x.Match(kDashAttackSpeed)) return fail();
  const auto run_whole = x.Word(0x94D3);
  const auto run_fraction = x.Word(0x94D7);
  if (!run_whole || !run_fraction) return fail();
  data.run_speed = Fixed16(*run_whole, *run_fraction);
  for (size_t t = 0; t < kTurtles; ++t) {
    const auto row = x.Word(uint16_t(0x94C1 + 2 * (t + 1)));
    if (!row || *row < 0 || *row > 0x200 || *row % 4 != 0) return fail();
    const auto speed = x.Word(uint16_t(0x9663 + *row));
    if (!speed) return fail();
    data.walk_speed[t] = int32_t(*speed) * 256;
  }
  const auto attack = x.Match(kDashAttack);
  const auto attack_whole = x.Word(0x96C7);
  const auto attack_fraction = x.Word(0x96D7);
  if (!attack || !attack_whole || !attack_fraction) return fail();
  data.dash_attack_frames = uint8_t((*attack)[0] + 1);
  data.dash_attack_speed = Fixed16(*attack_whole, *attack_fraction);
  if (data.run_speed <= 0 || data.dash_attack_speed <= 0 ||
      std::any_of(data.walk_speed.begin(), data.walk_speed.end(), [](int32_t s) { return s <= 0; })) {
    x.Invalid("a movement speed is not usable");
    return fail();
  }

  result.ok = true;
  return result;
}

std::vector<uint8_t> SerializeGameplay(const GameplayData& data, std::string_view source_rom_sha256) {
  std::vector<uint8_t> out(kMagic, kMagic + sizeof kMagic);
  PutU32(out, kGameplayFormatVersion);
  PutU32(out, kGameplayExtractorVersion);
  std::string sha(source_rom_sha256.substr(0, kShaHex));
  sha.resize(kShaHex, '0');
  out.insert(out.end(), sha.begin(), sha.end());
  PutU32(out, uint32_t(kPayloadSize));
  out.push_back(uint8_t(data.video));
  for (const Facing f : data.facing) out.push_back(uint8_t(f));
  out.push_back(data.lane_tolerance);
  out.push_back(data.grab_range);
  out.push_back(data.attack_search_range);
  out.insert(out.end(), data.back_attack_range.begin(), data.back_attack_range.end());
  out.push_back(data.throw_hold_frames);
  out.push_back(data.dash_hold_frames);
  out.push_back(data.dash_tap_frames);
  out.push_back(data.dash_auto_default ? 1 : 0);
  PutU32(out, uint32_t(data.run_speed));
  for (const int32_t s : data.walk_speed) PutU32(out, uint32_t(s));
  PutU32(out, uint32_t(data.dash_attack_speed));
  out.push_back(data.dash_attack_frames);
  const std::string checksum = trg::update::Sha256Hex(AsText(out));
  out.insert(out.end(), checksum.begin(), checksum.end());
  return out;
}

GameplayFileError ParseGameplay(std::span<const uint8_t> bytes, GameplayData& data, std::string& source_rom_sha256) {
  constexpr size_t kHeader = sizeof kMagic + 4 + 4 + kShaHex + 4;
  if (bytes.size() < kHeader || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) {
    return GameplayFileError::kMalformed;
  }
  if (GetU32(bytes, 8) != kGameplayFormatVersion || GetU32(bytes, 12) != kGameplayExtractorVersion) {
    return GameplayFileError::kOtherVersion;
  }
  if (GetU32(bytes, kHeader - 4) != kPayloadSize || bytes.size() != kHeader + kPayloadSize + kShaHex) {
    return GameplayFileError::kMalformed;
  }
  const std::span<const uint8_t> body = bytes.first(kHeader + kPayloadSize);
  if (trg::update::Sha256Hex(AsText(body)) != AsText(bytes.subspan(body.size()))) {
    return GameplayFileError::kChecksum;
  }
  const std::string sha = AsText(bytes.subspan(16, kShaHex));
  if (!IsShaHex(sha)) {
    return GameplayFileError::kMalformed;
  }
  GameplayData parsed;
  size_t at = kHeader;
  const auto u8 = [&]() { return bytes[at++]; };
  const auto i32 = [&]() {
    const int32_t v = int32_t(GetU32(bytes, at));
    at += 4;
    return v;
  };
  const uint8_t video = u8();
  if (video > uint8_t(VideoStandard::kPal)) return GameplayFileError::kMalformed;
  parsed.video = VideoStandard(video);
  for (Facing& f : parsed.facing) {
    const uint8_t v = u8();
    if (v > uint8_t(Facing::kRight)) return GameplayFileError::kMalformed;
    f = Facing(v);
  }
  parsed.lane_tolerance = u8();
  parsed.grab_range = u8();
  parsed.attack_search_range = u8();
  for (uint8_t& range : parsed.back_attack_range) range = u8();
  parsed.throw_hold_frames = u8();
  parsed.dash_hold_frames = u8();
  parsed.dash_tap_frames = u8();
  const uint8_t auto_dash = u8();
  if (auto_dash > 1 || parsed.dash_hold_frames == 0) return GameplayFileError::kMalformed;
  parsed.dash_auto_default = auto_dash == 1;
  parsed.run_speed = i32();
  for (int32_t& s : parsed.walk_speed) s = i32();
  parsed.dash_attack_speed = i32();
  parsed.dash_attack_frames = u8();
  data = parsed;
  source_rom_sha256 = sha;
  return GameplayFileError::kOk;
}

GameplayStatusResult QueryGameplayStatus(const fs::path& user_data_root) {
  GameplayStatusResult result;
  const classic::StatusResult soundtrack = classic::QuerySoundtrackStatus(user_data_root);
  if (soundtrack.status == classic::SoundtrackStatus::kNotImported) {
    return result;
  }
  result.status = GameplayStatus::kReimportRequired;
  if (soundtrack.status != classic::SoundtrackStatus::kReady) {
    result.detail = "the SNES ROM import is not usable: " + soundtrack.detail;
    return result;
  }
  const fs::path file = classic::SoundtrackFolder(user_data_root) / kGameplayFile;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    if (soundtrack.manifest.importer_version >= classic::kImporterVersion) {
      result.status = GameplayStatus::kUnsupportedRom;
      result.detail = "the gameplay code of this ROM is not a known one";
    } else {
      result.detail = "the SNES ROM was imported before SNES Gameplay existed";
    }
    return result;
  }
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec || size > kMaxGameplayFile) {
    result.detail = "the gameplay data cannot be read";
    return result;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  std::ifstream in(file, std::ios::binary);
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  if (in.gcount() != std::streamsize(bytes.size())) {
    result.detail = "the gameplay data cannot be read";
    return result;
  }
  std::string source;
  switch (ParseGameplay(bytes, result.data, source)) {
    case GameplayFileError::kOk: break;
    case GameplayFileError::kOtherVersion:
      result.detail = "the gameplay data was made by another version";
      return result;
    case GameplayFileError::kChecksum:
    case GameplayFileError::kMalformed:
      result.detail = "the gameplay data is damaged";
      return result;
  }
  if (source != soundtrack.manifest.source_rom_sha256) {
    result.detail = "the gameplay data was made from a different ROM";
    result.data = {};
    return result;
  }
  result.status = GameplayStatus::kReady;
  result.region = soundtrack.manifest.source_region;
  return result;
}

const char* GameplayStatusLabel(GameplayStatus status) {
  switch (status) {
    case GameplayStatus::kNotImported: return "Not imported";
    case GameplayStatus::kReady: return "Ready";
    case GameplayStatus::kReimportRequired: return "Re-import required";
    case GameplayStatus::kUnsupportedRom: return "Unsupported ROM";
  }
  return "Not imported";
}

}  // namespace tmnt::snes
