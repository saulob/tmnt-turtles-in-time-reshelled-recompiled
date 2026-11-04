#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <array>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "netplay/relay_frame.h"
#include "netplay/udp_transport.h"

namespace tmnt::netplay {

// Sees every datagram the socket receives before the game does; true keeps it from the game. `reply` sends from the
// same socket and may be used during the call.
class DatagramTap {
 public:
  virtual ~DatagramTap() = default;
  virtual bool Take(const Endpoint& from, const uint8_t* data, size_t size, Transport& reply) = 0;
};

// Hands one encoded relay frame to the online service's connection; false when it cannot take it now.
using RelaySend = std::function<bool(std::vector<uint8_t>&&)>;

inline constexpr size_t kRelaySampledSizes = 4;

// One remote participant's datagrams.
struct PeerTraffic {
  uint64_t udp_tx_packets = 0;
  uint64_t udp_rx_packets = 0;
  uint64_t relay_tx_packets = 0;
  uint64_t relay_rx_packets = 0;
  uint64_t relay_rejected = 0;
  uint64_t relay_dropped = 0;  // queued too long unread
};

// The current match's gameplay datagrams, whichever way they went.
struct MatchTraffic {
  uint64_t udp_tx_packets = 0;
  uint64_t udp_rx_packets = 0;
  uint64_t relay_tx_packets = 0;
  uint64_t relay_tx_bytes = 0;
  uint64_t relay_rx_packets = 0;
  uint64_t relay_rx_bytes = 0;
  uint64_t relay_rejected = 0;
  uint64_t relay_dropped = 0;
  std::vector<size_t> relay_tx_sizes;  // the first few payload sizes each way
  std::vector<size_t> relay_rx_sizes;
};

// The one UDP socket online play uses from its early bind (so Windows can prompt for network access before any match)
// through every match: STUN, punching, the Player Match lobby and the game's own session all share it, because the NAT
// mapping the peer was told about belongs to this socket alone. When a match has no direct path, the same Transport
// carries it over the online service's relay instead: the game then talks to RelayPeerEndpoint(slot) and never
// knows the difference. Every remote participant is independent: one on the relay does not move the others. Thread-safe.
class OnlineGameSocket final : public Transport {
 public:
  // Binds the first free port of `ports`. Once open it stays on that port: later calls never rebind.
  bool Open(std::initializer_list<uint16_t> ports, bool loopback_only = false);
  bool is_open() const;
  uint16_t bound_port() const;

  void SetTap(DatagramTap* tap);
  // Runs `use` with the socket held, so the tap's own state and sends stay consistent with Take.
  void Use(const std::function<void(Transport&)>& use);
  // Lets the tap see what has arrived when no session may be reading; game datagrams wait for Receive.
  void Pump();

  // The match's relay as participant `my_slot`: from now on its frames (those carrying `token` addressed to this slot)
  // are queued per sender, but only once that sender is selected do sends to RelayPeerEndpoint(sender) go through the
  // relay and do its datagrams come out of Receive. Replaces any earlier relay.
  void OpenRelay(uint32_t token, int my_slot, RelaySend send);
  void SelectRelay(int peer_slot);
  // Forgets one participant's relay (direct was chosen, or it left) and what it queued; the others are untouched.
  void CloseRelayPeer(int peer_slot);
  // Forgets the whole relay and everything it queued.
  void CloseRelay();
  bool relay_open() const;
  bool relay_selected(int peer_slot) const;
  // One binary message from the online service; false when it is not a frame of the open relay for this slot.
  bool DeliverRelay(const uint8_t* data, size_t size);

  // Tests simulate a firewall: direct datagrams to or from an endpoint it refuses are dropped.
  void SetDirectFilter(std::function<bool(const Endpoint&)> allow);

  // Direct datagrams from and to `endpoint` count for this participant.
  void SetPeerEndpoint(int peer_slot, const Endpoint& endpoint);

  // Counting starts again for each match.
  void ResetTraffic();
  MatchTraffic traffic() const;
  PeerTraffic peer_traffic(int peer_slot) const;

  bool Send(const Endpoint& to, const uint8_t* data, size_t size) override;
  size_t Receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;

 private:
  bool TakenByTap(const Endpoint& from, const uint8_t* data, size_t size);
  bool SendRelayLocked(int peer_slot, const uint8_t* data, size_t size);
  bool TapTakesRelayed(const Endpoint& from, const std::vector<uint8_t>& data, Transport& reply);

  // Answers a relayed datagram the tap took (a round-trip probe) back over the same relay, with the socket's lock held.
  class RelayReply final : public Transport {
   public:
    RelayReply(OnlineGameSocket& socket, int slot) : socket_(socket), slot_(slot) {}
    bool Send(const Endpoint&, const uint8_t* data, size_t size) override { return socket_.SendRelayLocked(slot_, data, size); }
    size_t Receive(uint8_t*, size_t, Endpoint&) override { return 0; }

   private:
    OnlineGameSocket& socket_;
    int slot_;
  };
  int PeerOfEndpoint(const Endpoint& endpoint) const;

  struct PeerRelay {
    bool selected = false;
    uint32_t seq = 0;
    std::deque<std::vector<uint8_t>> in;
    };

  mutable std::mutex mutex_;
  UdpTransport socket_;
  DatagramTap* tap_ = nullptr;
  std::deque<std::pair<Endpoint, std::vector<uint8_t>>> waiting_;

  uint32_t relay_token_ = 0;
  int relay_slot_ = 0;
  RelaySend relay_send_;
  std::array<PeerRelay, kMaxSlot + 1> relay_peers_;
  int next_relay_peer_ = 1;
  std::array<Endpoint, kMaxSlot + 1> peer_endpoints_{};
  std::function<bool(const Endpoint&)> direct_filter_;
  MatchTraffic traffic_;
  std::array<PeerTraffic, kMaxSlot + 1> peer_traffic_{};
};

}  // namespace tmnt::netplay
