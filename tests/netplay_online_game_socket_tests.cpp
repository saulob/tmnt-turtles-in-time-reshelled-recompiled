// OnlineGameSocket: the one UDP socket online play binds early and keeps for every match, and the relay that carries a
// match over the online service instead. Headless; 127.0.0.1 only, fixed test ports; the service is an in-process
// stand-in that routes frames the way the hub does.
// Run: tmnt_netplay_online_game_socket_tests

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "netplay/lobby_mux.h"
#include "netplay/online_game_socket.h"
#include "netplay/relay_frame.h"
#include "netplay/udp_transport.h"

using namespace tmnt::netplay;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

constexpr uint16_t kPortA = 58101;
constexpr uint16_t kPortB = 58102;
constexpr uint16_t kPeerPort = 58109;
constexpr uint32_t kLoopback = 0x7F000001;

void WaitForLoopback() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

// Consumes datagrams that start with "TAP", answering each from the same socket.
class EchoTap final : public DatagramTap {
 public:
  bool Take(const Endpoint& from, const uint8_t* data, size_t size, Transport& reply) override {
    if (size < 3 || std::memcmp(data, "TAP", 3) != 0) return false;
    ++taken;
    reply.Send(from, data, size);
    return true;
  }
  int taken = 0;
};

void Send(UdpTransport& from, uint16_t to_port, const char* text) {
  from.Send({kLoopback, to_port}, reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

std::string ReceiveText(Transport& transport, Endpoint& from) {
  uint8_t buffer[256];
  const size_t size = transport.Receive(buffer, sizeof buffer, from);
  return std::string(reinterpret_cast<const char*>(buffer), size);
}

void TestOpenTakesTheFirstFreePortAndNeverRebinds() {
  OnlineGameSocket first;
  CHECK(!first.is_open() && first.bound_port() == 0);
  CHECK(first.Open({kPortA, kPortB}, true));
  CHECK(first.is_open() && first.bound_port() == kPortA);

  // A second instance on this PC gets the other port.
  OnlineGameSocket second;
  CHECK(second.Open({kPortA, kPortB}, true) && second.bound_port() == kPortB);

  // Opening again changes nothing, whatever ports it names: a new bind would be a new NAT mapping.
  CHECK(first.Open({kPortB}, true) && first.bound_port() == kPortA);
  UdpTransport contender;
  CHECK(!contender.Open(kPortA, true));

  OnlineGameSocket third;
  CHECK(!third.Open({kPortA, kPortB}, true) && !third.is_open() && third.bound_port() == 0);
}

void TestTapSeesDatagramsFirstAndRepliesFromTheSameSocket() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  EchoTap tap;
  socket.SetTap(&tap);
  UdpTransport peer;
  CHECK(peer.Open(kPeerPort, true));

  Send(peer, kPortA, "TAP1");
  Send(peer, kPortA, "GAME1");
  WaitForLoopback();
  Endpoint from;
  CHECK(ReceiveText(socket, from) == "GAME1" && from == (Endpoint{kLoopback, kPeerPort}));
  CHECK(tap.taken == 1);
  WaitForLoopback();
  CHECK(ReceiveText(peer, from) == "TAP1" && from == (Endpoint{kLoopback, kPortA}));

  // What Use sends leaves from the bound port too.
  socket.Use([](Transport& out) { out.Send({kLoopback, kPeerPort}, reinterpret_cast<const uint8_t*>("USE"), 3); });
  WaitForLoopback();
  CHECK(ReceiveText(peer, from) == "USE" && from == (Endpoint{kLoopback, kPortA}));
  socket.SetTap(nullptr);
}

void TestPumpKeepsGameDatagramsInOrderForTheSession() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  EchoTap tap;
  socket.SetTap(&tap);
  UdpTransport peer;
  CHECK(peer.Open(kPeerPort, true));
  Send(peer, kPortA, "GAME1");
  Send(peer, kPortA, "TAP2");
  Send(peer, kPortA, "GAME2");
  WaitForLoopback();
  socket.Pump();
  CHECK(tap.taken == 1);
  Endpoint from;
  CHECK(ReceiveText(socket, from) == "GAME1");
  CHECK(ReceiveText(socket, from) == "GAME2");
  CHECK(ReceiveText(socket, from).empty());
  CHECK(socket.bound_port() == kPortA);
  socket.SetTap(nullptr);
}

constexpr uint32_t kToken = 0xC0FFEE01;

std::vector<uint8_t> Bytes(const char* text) { return {text, text + std::strlen(text)}; }

// What the relay hands the online service's connection.
struct Outbox {
  std::vector<std::vector<uint8_t>> frames;
  bool refuse = false;
  RelaySend Sender() {
    return [this](std::vector<uint8_t>&& frame) {
      if (refuse) return false;
      frames.push_back(std::move(frame));
      return true;
    };
  }
};

void TestRelayFramesEncodeAndDecode() {
  const std::vector<uint8_t> payload = Bytes("lobby!");
  const std::vector<uint8_t> frame = EncodeRelayFrame(2, 3, 0xDEADBEEF, 7, payload.data(), payload.size());
  CHECK(frame.size() == kRelayHeaderBytes + payload.size());
  CHECK(frame[0] == 'S' && frame[1] == 'R' && frame[2] == kRelayVersion && frame[3] == kRelayData);
  CHECK(frame[4] == 2 && frame[5] == 3);
  RelayFrameError error = RelayFrameError::kBadLength;
  const auto decoded = DecodeRelayFrame(frame.data(), frame.size(), &error);
  CHECK(decoded && decoded->src == 2 && decoded->dst == 3 && decoded->token == 0xDEADBEEF && decoded->seq == 7 &&
        decoded->payload == payload);
  CHECK(error == RelayFrameError::kNone);

  const auto reason = [&](std::vector<uint8_t> bytes, size_t size) {
    RelayFrameError why = RelayFrameError::kNone;
    CHECK(!DecodeRelayFrame(bytes.data(), size, &why));
    return std::string(RelayFrameErrorName(why));
  };
  std::vector<uint8_t> bad = frame;
  bad[0] = '{';
  CHECK(reason(bad, bad.size()) == "bad_magic");
  bad = frame;
  bad[2] = 1;
  CHECK(reason(bad, bad.size()) == "bad_version");
  bad = frame;
  bad[3] = 9;
  CHECK(reason(bad, bad.size()) == "bad_type");
  for (const uint8_t slot : {0, 5, 255}) {
    bad = frame;
    bad[4] = slot;
    CHECK(reason(bad, bad.size()) == "bad_slot");
    bad = frame;
    bad[5] = slot;
    CHECK(reason(bad, bad.size()) == "bad_slot");
  }
  CHECK(reason(frame, 3) == "truncated");
  CHECK(reason(frame, kRelayHeaderBytes - 1) == "truncated");
  CHECK(reason(frame, frame.size() - 1) == "bad_length");
  std::vector<uint8_t> longer = frame;
  longer.push_back(0);
  CHECK(reason(longer, longer.size()) == "bad_length");

  const std::vector<uint8_t> largest(kRelayMaxPayload, 0x5A);
  const std::vector<uint8_t> full = EncodeRelayFrame(1, 4, 1, 1, largest.data(), largest.size());
  CHECK(DecodeRelayFrame(full.data(), full.size()));
  const std::vector<uint8_t> over(kRelayMaxPayload + 1, 0x5A);
  CHECK(EncodeRelayFrame(1, 2, 1, 1, over.data(), over.size()).empty());
  CHECK(EncodeRelayFrame(1, 2, 1, 1, over.data(), 0).empty());
  CHECK(EncodeRelayFrame(0, 2, 1, 1, largest.data(), 4).empty() && EncodeRelayFrame(1, 5, 1, 1, largest.data(), 4).empty());
  std::vector<uint8_t> forged(kRelayHeaderBytes + kRelayMaxPayload + 1, 0);
  std::memcpy(forged.data(), full.data(), kRelayHeaderBytes);
  forged[14] = uint8_t((kRelayMaxPayload + 1) >> 8);
  forged[15] = uint8_t(kRelayMaxPayload + 1);
  CHECK(reason(forged, forged.size()) == "oversized");
}

void TestRelayEndpointsAreOnePerSlot() {
  for (int slot = 1; slot <= kMaxSlot; ++slot) {
    CHECK(RelayPeerSlot(RelayPeerEndpoint(slot)) == slot);
    CHECK(RelayPeerEndpoint(slot).address >> 24 == 127);
  }
  CHECK(RelayPeerSlot({kLoopback, 1}) == 0 && RelayPeerSlot({RelayPeerEndpoint(2).address, 2}) == 0);
  CHECK(RelayPeerSlot({0x7F0000F0u, 1}) == 0 && RelayPeerSlot({0x7F0000F0u + 5, 1}) == 0);
  CHECK(RelayPeerEndpoint(2) != RelayPeerEndpoint(3));
}

void TestRelayQueuesUntilSelectedAndSendsOnlyToTheRelayPeer() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  UdpTransport peer;
  CHECK(peer.Open(kPeerPort, true));
  Outbox outbox;
  const std::vector<uint8_t> hello = Bytes("HELLO");
  const std::vector<uint8_t> frame = EncodeRelayFrame(3, 2, kToken, 1, hello.data(), hello.size());

  CHECK(!socket.DeliverRelay(frame.data(), frame.size()));  // no relay yet
  socket.OpenRelay(kToken, 2, outbox.Sender());
  CHECK(socket.relay_open());
  CHECK(socket.DeliverRelay(frame.data(), frame.size()));
  Endpoint from;
  CHECK(ReceiveText(socket, from).empty());  // ready, not selected: nothing reaches the game yet
  CHECK(socket.Send(RelayPeerEndpoint(3), hello.data(), hello.size()));  // not selected: plain UDP, harmless loopback
  CHECK(outbox.frames.empty());

  socket.SelectRelay(3);
  CHECK(socket.relay_selected(3) && !socket.relay_selected(1) && !socket.relay_selected(4));
  CHECK(ReceiveText(socket, from) == "HELLO" && from == RelayPeerEndpoint(3));
  CHECK(socket.Send(RelayPeerEndpoint(3), hello.data(), hello.size()));
  CHECK(socket.Send(RelayPeerEndpoint(3), hello.data(), 2));
  CHECK(outbox.frames.size() == 2);
  const auto sent = DecodeRelayFrame(outbox.frames[0].data(), outbox.frames[0].size());
  const auto second = DecodeRelayFrame(outbox.frames[1].data(), outbox.frames[1].size());
  CHECK(sent && sent->src == 2 && sent->dst == 3 && sent->token == kToken && sent->seq == 1 && sent->payload == hello);
  CHECK(second && second->seq == 2 && second->payload.size() == 2);

  // Anything else still goes out as UDP, and UDP from elsewhere still comes in.
  CHECK(socket.Send({kLoopback, kPeerPort}, hello.data(), hello.size()));
  WaitForLoopback();
  CHECK(ReceiveText(peer, from) == "HELLO" && from == (Endpoint{kLoopback, kPortA}));
  CHECK(outbox.frames.size() == 2);

  const MatchTraffic traffic = socket.traffic();
  CHECK(traffic.relay_tx_packets == 2 && traffic.relay_tx_bytes == 7 && traffic.relay_rx_packets == 1);
  CHECK(traffic.relay_rx_bytes == 5 && traffic.udp_tx_packets == 2);
  CHECK(traffic.relay_tx_sizes == (std::vector<size_t>{5, 2}) && traffic.relay_rx_sizes == (std::vector<size_t>{5}));
  const PeerTraffic to_three = socket.peer_traffic(3);
  CHECK(to_three.relay_tx_packets == 2 && to_three.relay_rx_packets == 1 && socket.peer_traffic(2).relay_tx_packets == 0);
  socket.ResetTraffic();
  CHECK(socket.traffic().relay_tx_packets == 0 && socket.traffic().relay_tx_sizes.empty());
  CHECK(socket.peer_traffic(3).relay_tx_packets == 0);
}

void TestRelayRejectsOtherTokensSlotsAndForgetsEverythingOnClose() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  Outbox outbox;
  socket.OpenRelay(kToken, 1, outbox.Sender());
  socket.SelectRelay(2);
  const std::vector<uint8_t> data = Bytes("OLD");
  const std::vector<uint8_t> stale = EncodeRelayFrame(2, 1, kToken ^ 1, 1, data.data(), data.size());
  CHECK(!socket.DeliverRelay(stale.data(), stale.size()));
  const uint8_t junk[3] = {1, 2, 3};
  CHECK(!socket.DeliverRelay(junk, sizeof junk));
  const std::vector<uint8_t> misaddressed = EncodeRelayFrame(2, 3, kToken, 1, data.data(), data.size());
  CHECK(!socket.DeliverRelay(misaddressed.data(), misaddressed.size()));  // meant for another participant
  const std::vector<uint8_t> from_self = EncodeRelayFrame(1, 1, kToken, 1, data.data(), data.size());
  CHECK(!socket.DeliverRelay(from_self.data(), from_self.size()));
  CHECK(socket.traffic().relay_rejected == 4);
  Endpoint from;
  CHECK(ReceiveText(socket, from).empty());

  const std::vector<uint8_t> ours = EncodeRelayFrame(2, 1, kToken, 1, data.data(), data.size());
  CHECK(socket.DeliverRelay(ours.data(), ours.size()));
  socket.CloseRelay();
  CHECK(!socket.relay_open() && !socket.relay_selected(2));
  CHECK(ReceiveText(socket, from).empty());  // the queued frame went with the relay
  CHECK(!socket.DeliverRelay(ours.data(), ours.size()));
  CHECK(socket.Send(RelayPeerEndpoint(2), data.data(), data.size()));  // UDP to loopback now, never the old relay
  CHECK(outbox.frames.empty());

  // The next match's relay never sees the previous one's frames.
  socket.OpenRelay(kToken + 1, 1, outbox.Sender());
  socket.SelectRelay(2);
  CHECK(!socket.DeliverRelay(ours.data(), ours.size()));
  CHECK(ReceiveText(socket, from).empty());
}

void TestRelayFailsPerPeerOnOverflowAndRefusedSends() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  Outbox outbox;
  socket.OpenRelay(kToken, 1, outbox.Sender());
  socket.SelectRelay(2);
  socket.SelectRelay(3);
  const std::vector<uint8_t> data = Bytes("X");
  int accepted = 0;
  for (uint32_t i = 0; i < 300; ++i) {
    const std::vector<uint8_t> frame = EncodeRelayFrame(2, 1, kToken, i, data.data(), data.size());
    accepted += socket.DeliverRelay(frame.data(), frame.size()) ? 1 : 0;
  }
  CHECK(accepted == 256);
  CHECK(socket.relay_failure(2) == "receive_overflow" && socket.relay_failure(3).empty());
  Endpoint from;
  CHECK(ReceiveText(socket, from).empty());
  CHECK(!socket.Send(RelayPeerEndpoint(2), data.data(), data.size()));
  CHECK(socket.Send(RelayPeerEndpoint(3), data.data(), data.size()));  // the other participant is unaffected
  const std::vector<uint8_t> from_three = EncodeRelayFrame(3, 1, kToken, 1, data.data(), data.size());
  CHECK(socket.DeliverRelay(from_three.data(), from_three.size()) && ReceiveText(socket, from) == "X" &&
        from == RelayPeerEndpoint(3));

  socket.OpenRelay(kToken, 1, outbox.Sender());
  socket.SelectRelay(2);
  CHECK(socket.relay_failure(2).empty());
  outbox.refuse = true;
  CHECK(!socket.Send(RelayPeerEndpoint(2), data.data(), data.size()));
  CHECK(socket.relay_failure(2) == "send_refused");
}

// Three remote participants: one relayed, one direct, one relayed; closing one leaves the others as they were.
void TestPeersAreIndependent() {
  OnlineGameSocket socket;
  CHECK(socket.Open({kPortA}, true));
  UdpTransport direct_peer;
  CHECK(direct_peer.Open(kPeerPort, true));
  socket.SetPeerEndpoint(3, {kLoopback, kPeerPort});
  Outbox outbox;
  socket.OpenRelay(kToken, 1, outbox.Sender());
  socket.SelectRelay(2);
  socket.SelectRelay(4);
  const std::vector<uint8_t> data = Bytes("PING");

  CHECK(socket.Send(RelayPeerEndpoint(2), data.data(), data.size()));
  CHECK(socket.Send(RelayPeerEndpoint(4), data.data(), data.size()));
  CHECK(socket.Send({kLoopback, kPeerPort}, data.data(), data.size()));
  CHECK(outbox.frames.size() == 2);
  const auto first = DecodeRelayFrame(outbox.frames[0].data(), outbox.frames[0].size());
  const auto last = DecodeRelayFrame(outbox.frames[1].data(), outbox.frames[1].size());
  CHECK(first && first->dst == 2 && first->seq == 1 && last && last->dst == 4 && last->seq == 1);  // a sequence per peer
  WaitForLoopback();
  Endpoint from;
  CHECK(ReceiveText(direct_peer, from) == "PING");
  CHECK(socket.peer_traffic(3).udp_tx_packets == 1 && socket.peer_traffic(2).relay_tx_packets == 1 &&
        socket.peer_traffic(4).relay_tx_packets == 1 && socket.peer_traffic(3).relay_tx_packets == 0);

  Send(direct_peer, kPortA, "BACK");
  for (const uint8_t src : {2, 4}) {
    const std::vector<uint8_t> frame = EncodeRelayFrame(src, 1, kToken, 1, data.data(), data.size());
    CHECK(socket.DeliverRelay(frame.data(), frame.size()));
  }
  WaitForLoopback();
  std::vector<std::string> seen;
  std::vector<Endpoint> origins;
  for (int i = 0; i < 3; ++i) {
    const std::string text = ReceiveText(socket, from);
    CHECK(!text.empty());
    seen.push_back(text);
    origins.push_back(from);
  }
  CHECK(origins[0] == RelayPeerEndpoint(2) && origins[1] == RelayPeerEndpoint(4));  // relayed peers take turns first
  CHECK(origins[2] == (Endpoint{kLoopback, kPeerPort}) && seen[2] == "BACK");
  CHECK(socket.peer_traffic(3).udp_rx_packets == 1 && socket.peer_traffic(2).relay_rx_packets == 1);

  socket.CloseRelayPeer(2);
  CHECK(!socket.relay_selected(2) && socket.relay_selected(4) && socket.relay_open());
  CHECK(socket.Send(RelayPeerEndpoint(4), data.data(), data.size()) && outbox.frames.size() == 3);
}

// The service in miniature: a frame goes to its destination's socket unchanged, only when its source is the sender.
struct ServiceStandIn {
  struct Wire {
    std::deque<std::vector<uint8_t>> out;
  };
  std::array<Wire, kMaxSlot + 1> wires;
  RelaySend From(int slot) {
    return [this, slot](std::vector<uint8_t>&& frame) {
      wires[slot].out.push_back(std::move(frame));
      return true;
    };
  }
  void Deliver(std::array<OnlineGameSocket*, kMaxSlot + 1>& sockets) {
    for (int sender = 1; sender <= kMaxSlot; ++sender) {
      for (; !wires[sender].out.empty(); wires[sender].out.pop_front()) {
        const std::vector<uint8_t>& frame = wires[sender].out.front();
        const auto decoded = DecodeRelayFrame(frame.data(), frame.size());
        if (!decoded || decoded->src != sender || !sockets[decoded->dst]) continue;
        sockets[decoded->dst]->DeliverRelay(frame.data(), frame.size());
      }
    }
  }
};

// Through the relay each participant's game gets exactly the datagram another sent, from that participant's relayed
// address, and nothing travels over UDP.
void TestFourParticipantsExchangeDatagramsThroughTheRelay() {
  const uint16_t ports[kMaxSlot + 1] = {0, kPortA, kPortB, kPortA + 20, kPortB + 20};
  OnlineGameSocket sockets[kMaxSlot + 1];
  std::array<OnlineGameSocket*, kMaxSlot + 1> by_slot{};
  ServiceStandIn service;
  for (int slot = 1; slot <= kMaxSlot; ++slot) {
    CHECK(sockets[slot].Open({ports[slot]}, true));
    sockets[slot].OpenRelay(kToken, slot, service.From(slot));
    by_slot[slot] = &sockets[slot];
  }
  for (int slot = 1; slot <= kMaxSlot; ++slot) {
    for (int peer = 1; peer <= kMaxSlot; ++peer) {
      if (peer != slot) sockets[slot].SelectRelay(peer);
    }
  }
  for (int from_slot = 1; from_slot <= kMaxSlot; ++from_slot) {
    for (int to_slot = 1; to_slot <= kMaxSlot; ++to_slot) {
      if (from_slot == to_slot) continue;
      const std::vector<uint8_t> data = {uint8_t(from_slot), uint8_t(to_slot), 0x42};
      CHECK(sockets[from_slot].Send(RelayPeerEndpoint(to_slot), data.data(), data.size()));
    }
  }
  service.Deliver(by_slot);
  for (int slot = 1; slot <= kMaxSlot; ++slot) {
    std::set<int> senders;
    uint8_t buffer[16];
    Endpoint from;
    while (const size_t size = sockets[slot].Receive(buffer, sizeof buffer, from)) {
      CHECK(size == 3 && buffer[1] == slot && buffer[2] == 0x42 && from == RelayPeerEndpoint(buffer[0]));
      senders.insert(buffer[0]);
    }
    CHECK(senders.size() == 3 && !senders.count(slot));
    CHECK(sockets[slot].traffic().udp_tx_packets == 0 && sockets[slot].traffic().relay_rx_packets == 3);
  }
}

// A forged frame (wrong source) never reaches a game: the service-side check stops it, and the receiver's own
// destination check stops anything addressed elsewhere.
void TestSpoofedFramesNeverReachAGame() {
  OnlineGameSocket three, four;
  CHECK(three.Open({kPortA}, true) && four.Open({kPortB}, true));
  ServiceStandIn service;
  three.OpenRelay(kToken, 3, service.From(3));
  four.OpenRelay(kToken, 4, service.From(4));
  three.SelectRelay(4);
  four.SelectRelay(3);
  std::array<OnlineGameSocket*, kMaxSlot + 1> by_slot{};
  by_slot[3] = &three;
  by_slot[4] = &four;
  const std::vector<uint8_t> data = Bytes("FORGED");
  service.wires[4].out.push_back(EncodeRelayFrame(3, 4, kToken, 1, data.data(), data.size()));  // slot 4 claims to be 3
  service.Deliver(by_slot);
  Endpoint from;
  CHECK(ReceiveText(four, from).empty());
  const std::vector<uint8_t> elsewhere = EncodeRelayFrame(3, 2, kToken, 1, data.data(), data.size());
  CHECK(!four.DeliverRelay(elsewhere.data(), elsewhere.size()));
  CHECK(four.traffic().relay_rejected == 1 && ReceiveText(four, from).empty());
}

}  // namespace

int main() {
  std::printf("netplay online game socket tests\n");
  TestOpenTakesTheFirstFreePortAndNeverRebinds();
  TestTapSeesDatagramsFirstAndRepliesFromTheSameSocket();
  TestPumpKeepsGameDatagramsInOrderForTheSession();
  TestRelayFramesEncodeAndDecode();
  TestRelayEndpointsAreOnePerSlot();
  TestRelayQueuesUntilSelectedAndSendsOnlyToTheRelayPeer();
  TestRelayRejectsOtherTokensSlotsAndForgetsEverythingOnClose();
  TestRelayFailsPerPeerOnOverflowAndRefusedSends();
  TestPeersAreIndependent();
  TestFourParticipantsExchangeDatagramsThroughTheRelay();
  TestSpoofedFramesNeverReachAGame();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
