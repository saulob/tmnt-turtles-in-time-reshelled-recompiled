#include "fire_movie_patch.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace tmnt {
namespace {

// Takes the movie's PS3 branch, which hides the party row and ends the tab list at the third row.
constexpr MovieEdit kOnlineEdits[] = {
    {2112, "04", "03"},
    {6054, "1e", "00"},
    {6150, "04", "03"},
};

// Leaderboards leaves the tab order and is hidden; the rows below it move up one slot. Tab numbers stay: the game
// reads them as action ids.
constexpr MovieEdit kMainMenuEdits[] = {
    {1965,
     "9609000600000000000000008701000117960400040108091c48129d02002100960200080a1c960700040107010000004f960200040150",
     "960200080a96280007010000000701000000070000000007010000000701000000070100000007000000000707000000421d9902000a00"},
    {3114, "01000000", "00000000"},
    {10594, "1e00", "0000"},
    {10600, "14", "12"},
    {10616, "06000000", "07000000"},
    {67904, "1ee74d51e0", "1ee01538c0"},
    {68433, "1ee01538c0", "1ed97d2180"},
};

// Y does nothing, its hint is hidden and B moves into its slot.
constexpr MovieEdit kLobbyEdits[] = {
    {4320, "960400082f08001c96020008014e9602000802", "960200081b1c960400080905004f9902003600"},
    {7865, "9f03", "0000"},
    {67194, "1eed297a20", "1e85257a20"},
};

// Y does nothing and its hint is hidden.
constexpr MovieEdit kMatchmakingEdits[] = {
    {3780, "960400083208091c960200080a4e960200080b", "96020008281c960400082105004f9902003600"},
    {4389, "6c01", "0000"},
};

// Like the main menu: Leaderboards leaves the tab order and the rows below it move up one slot.
constexpr MovieEdit kPauseEdits[] = {
    {1956, "960c0007010000000701000000080a1c960200", "960200080d1c960400082405004f9902000400"},
    {3179, "960c0007010000000701000000080a1c960200", "960200080d1c960400082405004f9902000400"},
    {3513, "01000000", "00000000"},
    {4396, "960c0007010000000701000000080a1c960200", "960200080d1c960400082405004f9902000400"},
    {4671,
     "9609000600000000000000008701000117960400040108251c48129d0200210096020008121c960700040107010000004f96",
     "96020008129623000701000000070100000007010000000700000000070100000007010000000706000000421d9902000f00"},
    {64810, "1ea0b13b60", "1e97512060"},
    {65318, "1e97512060", "1e8e650570"},
    {65880, "1e8e650570", "1e84d8e760"},
};

// Y does nothing and its hint is hidden. The two remaining hints are never up together.
constexpr MovieEdit kSummaryEdits[] = {
    {2943, "96020008001c96020008014e96020008024e96", "960200081d1c960400080805004f9902002c00"},
    {4639, "6602", "0000"},
};

// Y does nothing, its hint is hidden and A moves into its slot.
constexpr MovieEdit kGameOverEdits[] = {
    {2620, "960200080a1c960200080b4e960200080c4e96", "96020008191c960400080205004f9902002c00"},
    {4077, "6f01", "0000"},
    {47386, "20947e5e10", "205be65e10"},
};

constexpr size_t kBundle0 = 19784989;  // f88d0000.bin
constexpr size_t kBundle2 = 15331229;  // f88d0002.bin

constexpr MenuMovie kMenuMovies[] = {
    {"online", kBundle0, 2200404, 115401, 0x54BD8CE213024D57ull, kOnlineEdits},
    {"menu_main", kBundle0, 1957054, 131133, 0x097851B1E2D30D5Dull, kMainMenuEdits},
    {"lobby", kBundle0, 1596592, 128626, 0x83B2A5DCE5128510ull, kLobbyEdits},
    {"matchmaking", kBundle0, 1845191, 111667, 0xF9BDFABCED4AAA21ull, kMatchmakingEdits},
    {"pause", kBundle2, 432, 125744, 0x643448261593C859ull, kPauseEdits},
    {"summary", kBundle2, 1801919, 126669, 0xCDD3DC93E1D2D4E9ull, kSummaryEdits},
    {"game_over", kBundle2, 639704, 129644, 0x43FA342A109A9A33ull, kGameOverEdits},
};

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

std::optional<uint8_t> HexDigit(char c) {
  if (c >= '0' && c <= '9') return uint8_t(c - '0');
  if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> ParseHex(std::string_view hex) {
  if (hex.size() % 2) return std::nullopt;
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const auto high = HexDigit(hex[i]);
    const auto low = HexDigit(hex[i + 1]);
    if (!high || !low) return std::nullopt;
    bytes.push_back(uint8_t(*high << 4 | *low));
  }
  return bytes;
}

}  // namespace

bool ApplyMovieEdits(uint8_t* movie, size_t size, std::span<const MovieEdit> edits) {
  if (!movie) return false;
  struct Resolved {
    size_t offset;
    std::vector<uint8_t> after;
  };
  std::vector<Resolved> resolved;
  resolved.reserve(edits.size());
  for (const MovieEdit& edit : edits) {
    const auto before = ParseHex(edit.before);
    auto after = ParseHex(edit.after);
    if (!before || !after || before->size() != after->size() || before->empty()) return false;
    if (edit.offset > size || before->size() > size - edit.offset) return false;
    if (!std::equal(before->begin(), before->end(), movie + edit.offset)) return false;
    resolved.push_back({edit.offset, std::move(*after)});
  }
  for (const Resolved& edit : resolved) std::copy(edit.after.begin(), edit.after.end(), movie + edit.offset);
  return true;
}

std::span<const MenuMovie> MenuMovies() { return kMenuMovies; }

bool IsMenuBundle(size_t size) { return size == kBundle0 || size == kBundle2; }

BundlePatchResult PatchMenuBundle(uint8_t* bundle, size_t size) {
  BundlePatchResult result;
  if (!bundle) return result;
  for (const MenuMovie& movie : kMenuMovies) {
    if (movie.bundle_bytes != size || movie.offset > size || movie.bytes > size - movie.offset) continue;
    uint8_t* bytes = bundle + movie.offset;
    const bool patched = Fnv1a64(bytes, movie.bytes) == movie.hash && ApplyMovieEdits(bytes, movie.bytes, movie.edits);
    ++(patched ? result.patched : result.failed);
  }
  return result;
}

}  // namespace tmnt
