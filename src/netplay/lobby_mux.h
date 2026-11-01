#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "netplay/relay_frame.h"
#include "netplay/udp_transport.h"

namespace tmnt::netplay {

// One datagram of the game's own Player Match lobby traffic, between two guest VDP ports.
struct LobbyDatagram {
  uint16_t from_port = 0;
  uint16_t to_port = 0;
  std::vector<uint8_t> data;
};

inline constexpr size_t kLobbyHeaderBytes = 12;
inline constexpr size_t kMaxLobbyPayload = 1400;

std::vector<uint8_t> EncodeLobby(uint32_t tag, const LobbyDatagram& datagram);
// Nothing for anything that is not a lobby datagram.
std::optional<std::pair<uint32_t, LobbyDatagram>> DecodeLobby(const uint8_t* data, size_t size);

inline constexpr size_t kMaxHeldLobby = 32;

// Lobby datagrams the game sends to a participant before its transport is selected are held rather than lost, so the
// game's own resend timer is not what delays the lobby once the transport is selected. Holds one match's datagrams at a
// time, for all participants.
class LobbyHold {
 public:
  // False when there is no match (`tag` 0), no such participant, or the hold is full.
  bool Hold(uint32_t tag, int slot, const LobbyDatagram& datagram);
  // The held datagrams of `tag` for `slot`, oldest first; datagrams of any other match are dropped.
  std::vector<LobbyDatagram> Take(uint32_t tag, int slot);
  void Clear();
  size_t size() const { return held_.size(); }

 private:
  uint32_t tag_ = 0;
  std::deque<std::pair<int, LobbyDatagram>> held_;
};

// Carries lobby datagrams on the gameplay socket, to and from every remote participant. Gameplay traffic passes through
// unchanged; lobby datagrams are accepted only with the current match's tag and from a participant's own endpoint.
// Thread-safe: the guest's network thread and the presenting thread may both use it.
class LobbyMux final : public Transport {
 public:
  explicit LobbyMux(Transport& inner) : inner_(inner) {}

  void SetMatch(uint32_t tag);
  // Where the participant in `slot` is reached: its direct endpoint, or RelayPeerEndpoint(slot).
  void SetPeer(int slot, const Endpoint& peer);
  void ClearPeer(int slot);
  bool HasPeer(int slot) const;
  bool SendLobby(int slot, const LobbyDatagram& datagram);
  // The oldest waiting datagram from any participant; `slot` says which.
  bool TakeLobby(int& slot, LobbyDatagram& out);

  bool Send(const Endpoint& to, const uint8_t* data, size_t size) override;
  size_t Receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;

  uint32_t lobby_sent() const;
  uint32_t lobby_received() const;
  uint32_t lobby_rejected() const;
  uint32_t lobby_received_from(int slot) const;

 private:
  void DrainLocked();
  int SlotOf(const Endpoint& from) const;

  Transport& inner_;
  mutable std::mutex mutex_;
  uint32_t tag_ = 0;
  std::array<Endpoint, kMaxSlot + 1> peers_{};
  std::deque<std::pair<int, LobbyDatagram>> lobby_;
  std::deque<std::pair<Endpoint, std::vector<uint8_t>>> game_;
  uint32_t sent_ = 0;
  uint32_t received_ = 0;
  uint32_t rejected_ = 0;
  std::array<uint32_t, kMaxSlot + 1> received_from_{};
};

}  // namespace tmnt::netplay
