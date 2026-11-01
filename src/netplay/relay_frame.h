#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "netplay/udp_transport.h"

namespace tmnt::netplay {

// One gameplay datagram between two participants of a match, carried over the online service's WebSocket as a binary
// message: "SR", version, type, source slot, destination slot, token (u32), sequence (u32), payload length (u16),
// payload; big-endian. The service checks the source against the socket it came from.
inline constexpr uint8_t kRelayVersion = 2;
inline constexpr uint8_t kRelayData = 1;
inline constexpr size_t kRelayHeaderBytes = 16;
// The largest IPv4 UDP payload in a 1500-byte Ethernet frame.
inline constexpr size_t kRelayMaxPayload = 1472;

inline constexpr int kMaxSlot = 4;

// Where the game sees the relayed participant in `slot` (1-4). Loopback, so a datagram that misses the relay is harmless.
constexpr Endpoint RelayPeerEndpoint(int slot) { return Endpoint{0x7F0000F0u + uint32_t(slot), 1}; }
constexpr int RelayPeerSlot(const Endpoint& endpoint) {
  return endpoint.port == 1 && endpoint.address > 0x7F0000F0u && endpoint.address <= 0x7F0000F0u + kMaxSlot
             ? int(endpoint.address - 0x7F0000F0u)
             : 0;
}

struct RelayFrame {
  uint8_t src = 0;
  uint8_t dst = 0;
  uint32_t token = 0;
  uint32_t seq = 0;
  std::vector<uint8_t> payload;
};

enum class RelayFrameError { kNone, kTruncated, kBadMagic, kBadVersion, kBadType, kOversized, kBadLength, kBadSlot };

const char* RelayFrameErrorName(RelayFrameError error);

// Empty for a payload that is empty or over kRelayMaxPayload, or a slot outside 1-4.
std::vector<uint8_t> EncodeRelayFrame(uint8_t src, uint8_t dst, uint32_t token, uint32_t seq, const uint8_t* payload,
                                      size_t size);
std::optional<RelayFrame> DecodeRelayFrame(const uint8_t* data, size_t size, RelayFrameError* error = nullptr);

}  // namespace tmnt::netplay
