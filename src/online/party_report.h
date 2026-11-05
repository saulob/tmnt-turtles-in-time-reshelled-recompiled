// Party snapshots for the service's optional party.report (server/tmnt-lobby/README.md, Party snapshots): which turtles
// the players of an online party chose. The game hook only reads character select state; nothing here writes to the game.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace tmnt::online {

inline constexpr int kPartySeats = 4;
// The game's actor ids: ACTOR_LEONARDO 0, ACTOR_MICHELANGELO 1, ACTOR_RAPHAEL 2, ACTOR_DONATELLO 3. PARTY_CHARACTERS in
// server/tmnt-lobby/src/party.js names them.
inline constexpr int kTurtleCount = 4;
inline constexpr int32_t kNoStation = -1;
inline constexpr size_t kMinPartyReported = 2;  // a party of one is every online player's idle state
inline constexpr double kPartyReportGapSeconds = 60.0;

constexpr bool ValidTurtle(int32_t turtle) { return turtle >= 0 && turtle < kTurtleCount; }

// One seat of the character select screen: m_PlayerChoice, m_PlayerConfirmed and m_PlayerStationId.
struct PartySeat {
  int32_t choice = -1;
  int32_t confirmed = 0;
  int32_t station = kNoStation;
};

using PartySeats = std::array<PartySeat, kPartySeats>;

struct PartySnapshot {
  std::array<uint8_t, kPartySeats> characters{};  // ascending
  uint8_t size = 0;
  int8_t host = -1;

  bool operator==(const PartySnapshot&) const = default;
};

inline size_t BookedSeats(const PartySeats& seats) {
  return size_t(std::count_if(seats.begin(), seats.end(), [](const PartySeat& seat) { return seat.station != kNoStation; }));
}

// The turtle of every booked seat, or nothing unless two or more seats are booked and each confirmed a valid turtle. Only
// a player's controller books a seat, so a seat is always a human. `host` is the first seat of `local_station`, which on
// the session master is the host's own.
inline std::optional<PartySnapshot> BuildPartySnapshot(const PartySeats& seats, int32_t local_station) {
  if (BookedSeats(seats) < kMinPartyReported) return std::nullopt;
  PartySnapshot snapshot;
  for (const PartySeat& seat : seats) {
    if (seat.station == kNoStation) continue;
    if (!ValidTurtle(seat.choice) || !seat.confirmed) return std::nullopt;
    if (snapshot.host < 0 && seat.station == local_station) snapshot.host = int8_t(seat.choice);
    snapshot.characters[snapshot.size++] = uint8_t(seat.choice);
  }
  if (snapshot.host < 0) return std::nullopt;
  std::sort(snapshot.characters.begin(), snapshot.characters.begin() + snapshot.size);
  return snapshot;
}

// The exe and TMNT_r.dll share no memory but rexruntime's cvars, so a snapshot crosses as one integer: 3 bits per seat
// (turtle + 1), 3 for the host (turtle + 1), then a sequence that makes every match start a change.
inline constexpr int kPartySeatBits = 3;
inline constexpr int kPartySequenceShift = (kPartySeats + 1) * kPartySeatBits;
inline constexpr uint32_t kPartySequenceMask = 0xffff;

constexpr int32_t EncodePartySnapshot(const PartySnapshot& snapshot, uint32_t sequence) {
  uint32_t bits = 0;
  for (int i = 0; i < snapshot.size; ++i) bits |= uint32_t(snapshot.characters[size_t(i)] + 1) << (i * kPartySeatBits);
  bits |= uint32_t(snapshot.host + 1) << (kPartySeats * kPartySeatBits);
  return int32_t(bits | ((sequence & kPartySequenceMask) << kPartySequenceShift));
}

inline std::optional<PartySnapshot> DecodePartySnapshot(int32_t encoded) {
  if (encoded <= 0) return std::nullopt;
  const uint32_t bits = uint32_t(encoded);
  constexpr uint32_t kField = (1u << kPartySeatBits) - 1;
  PartySnapshot snapshot;
  for (int i = 0; i < kPartySeats; ++i) {
    const uint32_t field = (bits >> (i * kPartySeatBits)) & kField;
    if (field == 0) break;
    if (!ValidTurtle(int32_t(field) - 1)) return std::nullopt;
    snapshot.characters[snapshot.size++] = uint8_t(field - 1);
  }
  const uint32_t host = (bits >> (kPartySeats * kPartySeatBits)) & kField;
  if (host == 0 || !ValidTurtle(int32_t(host) - 1) || snapshot.size < kMinPartyReported) return std::nullopt;
  snapshot.host = int8_t(host - 1);
  return snapshot;
}

// Only the host of a real party, signed in over a usable link, reports it.
constexpr bool CanReportParty(bool connected, bool in_party, int my_slot, size_t members) {
  return connected && in_party && my_slot == 1 && members >= kMinPartyReported;
}

// Decides when an offered snapshot is sent: not again for the same party, and not sooner than the gap after the last one.
// Volatile; nothing identifies a player.
class PartyReporter {
 public:
  void Offer(const PartySnapshot& snapshot) { pending_ = snapshot; }
  void Clear() { pending_.reset(); }
  bool pending() const { return pending_.has_value(); }

  std::optional<PartySnapshot> Take(const std::string& match, double now) {
    if (!pending_) return std::nullopt;
    if (sent_ && sent_match_ == match && *sent_ == *pending_) {
      pending_.reset();
      return std::nullopt;
    }
    if (sent_ && now - sent_at_ < kPartyReportGapSeconds) return std::nullopt;
    sent_ = pending_;
    sent_match_ = match;
    sent_at_ = now;
    pending_.reset();
    return sent_;
  }

 private:
  std::optional<PartySnapshot> pending_;
  std::optional<PartySnapshot> sent_;
  std::string sent_match_;
  double sent_at_ = 0;
};

}  // namespace tmnt::online
