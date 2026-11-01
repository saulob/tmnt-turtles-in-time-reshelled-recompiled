#pragma once

#include <cstdint>
#include <string>

namespace tmnt::online {

// How one peer relationship's datagrams travel. Chosen once per pair, before the game is told the peer, and kept.
enum class MatchTransport { kPending, kDirectProbing, kDirect, kRelay, kFailed };

const char* MatchTransportName(MatchTransport transport);

// What the game runtime reports about its traffic with one peer.
enum class NetplayLink { kNone, kConnecting, kConnected, kFighting };

// How the game should reach one remote participant, once the transport for that pair is chosen and proven.
struct NetplayAssignment {
  int slot = 0;
  std::string peer_ip;
  uint16_t peer_port = 0;
  uint32_t generation = 0;  // this process's count of matches; a session never outlives its match
  bool relay = false;       // the peer is netplay::RelayPeerEndpoint(slot) on the gameplay socket's relay
};

}  // namespace tmnt::online
