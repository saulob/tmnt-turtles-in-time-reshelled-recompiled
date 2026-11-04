#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "netplay/lobby_mux.h"
#include "online/online_service.h"

namespace tmnt {

// The guest addresses a Player Match peer by a synthetic IN_ADDR in 10.83.0.0/16 derived from its virtual XUID; it
// never sees a real address. Host byte order.
inline constexpr uint32_t kSyntheticNet = 0x0A530000;
uint32_t SyntheticAddress(uint64_t xuid);
constexpr bool IsSyntheticAddress(uint32_t address) { return (address & 0xFFFF0000u) == kSyntheticNet; }

// A room member, in the terms every console of the room shares.
struct AddressMember {
  int slot = 0;
  uint64_t xuid = 0;
};

// The synthetic addresses of the current room. Two XUIDs can hash to one address, and the game could not tell those
// members apart, so a member whose address is taken gets the next free one. Consoles walk the roster in slot order, which
// is the same everywhere, so they all agree; the host, slot 1, always has its hashed address, the one its search result
// shows. A member keeps its address until it leaves.
class SyntheticAddressBook {
 public:
  void Update(std::vector<AddressMember> roster);
  // A member's address; outside the room, the hashed one.
  uint32_t AddressOf(uint64_t xuid) const;

 private:
  std::map<uint64_t, uint32_t> assigned_;
};

// XNADDR: ina (4), inaOnline (4), wPortOnline (2), abEnet (6), abOnline (20).
inline constexpr uint32_t kXnAddrBytes = 36;
void WriteXnAddr(uint8_t* xnaddr, uint64_t xuid);
void WriteXnAddr(uint8_t* xnaddr, uint64_t xuid, uint32_t address);
// The XUID of an XNADDR written by WriteXnAddr.
std::optional<uint64_t> XnAddrXuid(const uint8_t* xnaddr);

uint32_t SessionTag(std::string_view match);

// One other participant of the party, as the guest and the mux see it.
struct PeerRoute {
  int slot = 0;
  uint32_t address = 0;
  uint64_t xuid = 0;
  uint32_t tag = 0;
  bool host = false;
};
std::optional<PeerRoute> RouteFor(const online::ActivePeer& peer, const SyntheticAddressBook& book = {});
// Brings the book up to date with this console (at `my_slot`, when it has one) and `peers`.
std::vector<PeerRoute> RoutesFor(const std::vector<online::ActivePeer>& peers, int my_slot, uint64_t my_xuid,
                                 SyntheticAddressBook& book);
const PeerRoute* RouteForAddress(const std::vector<PeerRoute>& routes, uint32_t address);

// XNQOS: cxnqos, cxnqosPending, then one XNQOSINFO (24 bytes) per probed address, then the probe data.
inline constexpr uint32_t kQosHeaderBytes = 8;
inline constexpr uint32_t kQosInfoBytes = 24;
inline constexpr uint8_t kQosComplete = 0x01;
inline constexpr uint8_t kQosTargetContacted = 0x02;
inline constexpr uint8_t kQosTargetDisabled = 0x04;
inline constexpr uint8_t kQosDataReceived = 0x08;
// No probe has run before the join; the game only needs a plausible value.
inline constexpr uint16_t kUnmeasuredRttMs = 50;

struct QosTarget {
  bool reachable = false;
  std::vector<uint8_t> data;
  uint16_t rtt_ms = 0;
};
uint32_t QosResultBytes(const std::vector<QosTarget>& targets);
// `out` is the host view of guest address `address`, at least QosResultBytes() long.
void WriteQosResult(uint8_t* out, uint32_t address, const std::vector<QosTarget>& targets);
// The probe target for an XNADDR: a friend's advertised Player Match with its host's QoS listen data.
QosTarget QosTargetFor(const uint8_t* xnaddr, const std::vector<online::PlayerMatchAd>& ads);

// Winsock values the shim answers with.
inline constexpr uint32_t kSockDgram = 2;
inline constexpr uint32_t kVdpProtocol = 254;
inline constexpr uint32_t kWouldBlock = 10035;   // WSAEWOULDBLOCK
inline constexpr uint32_t kIoIncomplete = 996;   // WSA_IO_INCOMPLETE
inline constexpr uint32_t kIoPending = 997;      // WSA_IO_PENDING
inline constexpr uint32_t kStatusPending = 0x103;

// Datagram sockets the shim takes over while online: VDP, and plain UDP, which the title may also use for the peer.
constexpr bool ShimSocket(uint32_t type, uint32_t protocol) {
  return type == kSockDgram && (protocol == kVdpProtocol || protocol == 17 || protocol == 0);
}

// XWSABUF arrays in guest memory (length, then pointer, big-endian): one datagram gathered from them, or scattered
// into them. `base` is the guest memory base; the arrays and their buffers are guest addresses.
std::vector<uint8_t> GatherBuffers(uint8_t* base, uint32_t buffers, uint32_t count);
uint32_t ScatterBuffers(uint8_t* base, uint32_t buffers, uint32_t count, const std::vector<uint8_t>& data);

// XWSAOVERLAPPED: Internal (status), InternalHigh (bytes), Offset, OffsetHigh, hEvent.
inline constexpr uint32_t kOverlappedStatus = 0;
inline constexpr uint32_t kOverlappedBytes = 4;
inline constexpr uint32_t kOverlappedEvent = 16;

// A WSARecvFrom the game left pending on a shim socket; the next datagram for that socket completes it.
struct PendingReceive {
  uint32_t handle = 0;
  uint32_t buffers = 0;
  uint32_t buffer_count = 0;
  uint32_t bytes = 0;  // lpNumberOfBytesRecvd
  uint32_t from = 0;
  uint32_t from_length = 0;
  uint32_t overlapped = 0;
};

// Oldest first per socket; a closed socket's receives are dropped.
// WSAGetLastError repeats its answer until the next socket call: the title reads it twice for one failed receive.
class SocketLastError {
 public:
  void Clear() { value_ = 0; }
  void Set(uint32_t error) { value_ = error; }
  uint32_t Get() const { return value_; }

 private:
  uint32_t value_ = 0;
};

class PendingReceives {
 public:
  // An overlapped carries one receive: a new one replaces it, so a datagram never lands in an abandoned buffer.
  void Add(const PendingReceive& receive);
  std::optional<PendingReceive> Take(uint32_t handle);
  void Drop(uint32_t handle);
  bool Has(uint32_t handle) const;
  std::vector<uint32_t> handles() const;
  size_t size() const { return pending_.size(); }

 private:
  std::deque<PendingReceive> pending_;
};

// Where the shim sends and takes lobby datagrams, per participant.
class LobbyLink {
 public:
  virtual ~LobbyLink() = default;
  virtual bool Send(int slot, const netplay::LobbyDatagram& datagram) = 0;
  virtual bool Take(int& slot, netplay::LobbyDatagram& out) = 0;
};

// The title's datagram sockets while an online match runs: guest handles that never reach a host socket. A datagram to
// a participant's synthetic address goes through the LobbyLink; received ones reach the socket bound to their port, or
// the only shim socket when none is bound to it.
class VdpShim {
 public:
  struct Received {
    uint32_t from_address = 0;
    uint16_t from_port = 0;
    std::vector<uint8_t> data;
  };

  uint32_t Create();
  bool Owns(uint32_t handle) const { return sockets_.count(handle) != 0; }
  void Bind(uint32_t handle, uint16_t port);
  void Close(uint32_t handle);
  // False when the datagram was dropped (no active match, or not addressed to a participant).
  bool SendTo(uint32_t handle, uint32_t to_address, uint16_t to_port, const uint8_t* data, size_t size,
              const std::vector<PeerRoute>& routes, LobbyLink& link);
  std::optional<Received> RecvFrom(uint32_t handle, const std::vector<PeerRoute>& routes, LobbyLink& link);
  // True when a datagram waits for the socket, after taking in what the link has.
  bool Readable(uint32_t handle, const std::vector<PeerRoute>& routes, LobbyLink& link);
  void Connect(uint32_t handle, uint32_t address, uint16_t port);
  bool Peer(uint32_t handle, uint32_t& address, uint16_t& port) const;
  uint16_t bound_port(uint32_t handle) const;
  size_t queued(uint32_t handle) const;
  // DEBUG_SAULO: the deepest socket queue, and the datagrams lost to a full queue so far.
  size_t deepest_queue() const;
  uint64_t dropped() const { return dropped_; }

 private:
  struct Queued {
    uint32_t from_address = 0;
    netplay::LobbyDatagram datagram;
  };
  struct Socket {
    uint16_t port = 0;
    bool connected = false;
    uint32_t peer_address = 0;
    uint16_t peer_port = 0;
    std::deque<Queued> queue;
  };
  void Follow(const std::vector<PeerRoute>& routes);
  void TakeIn(const std::vector<PeerRoute>& routes, LobbyLink& link);

  static constexpr uint32_t kFirstHandle = 0x5D500001;
  uint32_t next_ = kFirstHandle;
  uint32_t tag_ = 0;
  uint64_t dropped_ = 0;
  std::map<uint32_t, Socket> sockets_;
};

}  // namespace tmnt
