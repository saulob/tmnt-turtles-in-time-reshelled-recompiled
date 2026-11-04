#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "netplay/lobby_mux.h"
#include "netplay/udp_transport.h"
#include "netplay_setting.h"
#include "player_match.h"
#include "player_match_net.h"
#include "socket_select.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(expr)) {                                                  \
      ++g_failures;                                                 \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    }                                                               \
  } while (0)

using namespace tmnt;
namespace np = tmnt::netplay;

constexpr char kHostCode[] = "AAAA-2222";
constexpr char kJoinCode[] = "BBBB-3333";

constexpr char kThirdCode[] = "CCCC-4444";
constexpr char kFourthCode[] = "DDDD-5555";

online::ActivePeer Peer(const char* match, int slot, const char* player, bool host) {
  return online::ActivePeer{match, slot, player, "", host};
}

std::vector<PeerRoute> Routes(std::initializer_list<online::ActivePeer> peers) {
  SyntheticAddressBook book;
  return RoutesFor(std::vector<online::ActivePeer>(peers), 0, 0, book);
}

class FakeLink final : public LobbyLink {
 public:
  bool Send(int slot, const np::LobbyDatagram& d) override {
    sent.emplace_back(slot, d);
    return true;
  }
  bool Take(int& slot, np::LobbyDatagram& d) override {
    if (inbox.empty()) return false;
    slot = inbox.front().first;
    d = inbox.front().second;
    inbox.pop_front();
    return true;
  }
  std::vector<std::pair<int, np::LobbyDatagram>> sent;
  std::deque<std::pair<int, np::LobbyDatagram>> inbox;
};

class FakeTransport final : public np::Transport {
 public:
  bool Send(const np::Endpoint& to, const uint8_t* data, size_t size) override {
    sent.emplace_back(to, std::vector<uint8_t>(data, data + size));
    return true;
  }
  size_t Receive(uint8_t* buffer, size_t capacity, np::Endpoint& from) override {
    if (inbox.empty()) return 0;
    from = inbox.front().first;
    const size_t n = std::min(capacity, inbox.front().second.size());
    std::memcpy(buffer, inbox.front().second.data(), n);
    inbox.pop_front();
    return n;
  }
  std::vector<std::pair<np::Endpoint, std::vector<uint8_t>>> sent;
  std::deque<std::pair<np::Endpoint, std::vector<uint8_t>>> inbox;
};

std::string ReadPath(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string ReadSource(const char* name) {
  std::string source = ReadPath(std::string(TMNT_SOURCE_DIR) + "/src/" + name);
  source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
  return source;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestActiveMatchCreatesARoute() {
  CHECK(!RouteFor(online::ActivePeer{}));
  CHECK(!RouteFor(Peer("", 2, kJoinCode, true)));
  CHECK(!RouteFor(Peer("m1", 0, kJoinCode, true)));
  CHECK(!RouteFor(Peer("m1", 5, kJoinCode, true)));
  CHECK(!RouteFor(Peer("m1", 2, "", true)));
  const auto host_view = RouteFor(Peer("m1", 2, kJoinCode, false));
  const auto join_view = RouteFor(Peer("m1", 1, kHostCode, true));
  CHECK(host_view && join_view);
  CHECK(host_view->slot == 2 && join_view->slot == 1);
  CHECK(host_view->address == SyntheticAddress(VirtualXuid(kJoinCode)));
  CHECK(join_view->address == SyntheticAddress(VirtualXuid(kHostCode)));
  CHECK(host_view->tag == join_view->tag && host_view->tag == SessionTag("m1"));
  CHECK(!host_view->host && join_view->host);
  CHECK(RouteFor(Peer("m2", 2, kJoinCode, false))->tag != host_view->tag);

  const auto routes = Routes({Peer("m1", 2, kJoinCode, false), Peer("m1", 3, kThirdCode, false),
                              Peer("m1", 4, kFourthCode, false), Peer("", 1, kHostCode, true)});
  CHECK(routes.size() == 3);
  const PeerRoute* third = RouteForAddress(routes, SyntheticAddress(VirtualXuid(kThirdCode)));
  CHECK(third && third->slot == 3 && !RouteForAddress(routes, SyntheticAddress(VirtualXuid(kHostCode))));
  CHECK(routes[0].address != routes[1].address && routes[1].address != routes[2].address &&
        routes[0].address != routes[2].address);
}

void TestXnAddrMapsBothWays() {
  uint8_t xnaddr[kXnAddrBytes];
  const uint64_t xuid = VirtualXuid(kHostCode);
  WriteXnAddr(xnaddr, xuid);
  CHECK(XnAddrXuid(xnaddr) == xuid);
  const uint32_t address = LoadBE32(xnaddr, 0);
  CHECK(address == SyntheticAddress(xuid) && LoadBE32(xnaddr, 4) == address && IsSyntheticAddress(address));
  CHECK((address & 0xFF) != 0 && (address & 0xFF) != 255 && ((address >> 8) & 0xFF) != 0);
  CHECK(xnaddr[10] == uint8_t(xuid >> 40) && xnaddr[15] == uint8_t(xuid));
  uint8_t reverse[kXnAddrBytes];
  const auto route = RouteFor(Peer("m1", 1, kHostCode, true));
  WriteXnAddr(reverse, route->xuid);
  CHECK(std::memcmp(reverse, xnaddr, kXnAddrBytes) == 0);
  uint8_t foreign[kXnAddrBytes] = {};
  StoreBE32(foreign, 0, 0x7F000001);
  CHECK(!XnAddrXuid(foreign));
  CHECK(!IsSyntheticAddress(0x7F000001) && !IsSyntheticAddress(0xC0A80105));
}

void TestQosReturnsTheAdvertisedPeer() {
  online::PlayerMatchAd ad;
  ad.player = kHostCode;
  ad.qos = std::string(224, 'a');
  uint8_t host[kXnAddrBytes], stranger[kXnAddrBytes];
  WriteXnAddr(host, VirtualXuid(kHostCode));
  WriteXnAddr(stranger, VirtualXuid("CCCC-4444"));
  const QosTarget found = QosTargetFor(host, {ad});
  CHECK(found.reachable && found.data.size() == 112 && found.rtt_ms == kUnmeasuredRttMs);
  CHECK(!QosTargetFor(stranger, {ad}).reachable);
  CHECK(!QosTargetFor(host, {}).reachable);

  const std::vector<QosTarget> targets = {found, QosTargetFor(stranger, {ad})};
  constexpr uint32_t kAddress = 0x40002000;
  std::vector<uint8_t> out(QosResultBytes(targets), 0xCC);
  CHECK(out.size() == kQosHeaderBytes + 2 * kQosInfoBytes + 112);
  WriteQosResult(out.data(), kAddress, targets);
  CHECK(LoadBE32(out.data(), 0) == 2 && LoadBE32(out.data(), 4) == 0);
  const uint8_t* first = out.data() + kQosHeaderBytes;
  CHECK(first[0] == (kQosComplete | kQosTargetContacted | kQosDataReceived));
  CHECK(LoadBE16(first, 2) == 1 && LoadBE16(first, 4) == 1 && LoadBE16(first, 6) == 112);
  CHECK(LoadBE32(first, 8) == kAddress + kQosHeaderBytes + 2 * kQosInfoBytes);
  CHECK(LoadBE16(first, 12) == kUnmeasuredRttMs && LoadBE32(first, 16) == 0 && LoadBE32(first, 20) == 0);
  CHECK(out[kQosHeaderBytes + 2 * kQosInfoBytes] == 0xAA);
  const uint8_t* second = first + kQosInfoBytes;
  CHECK(second[0] == (kQosComplete | kQosTargetDisabled) && LoadBE16(second, 6) == 0);
  // The game's consumer keeps a result only with COMPLETE and DATA_RECEIVED, without TARGET_DISABLED, and 112 bytes.
  const auto kept = [](const uint8_t* info) {
    return (info[0] & 0x01) && (info[0] & 0x08) && !(info[0] & 0x04) && LoadBE16(info, 6) == 112;
  };
  CHECK(kept(first) && !kept(second));
}

void TestShimRoutesOnlyToTheParty() {
  VdpShim shim;
  FakeLink link;
  const uint32_t lobby = shim.Create();
  const uint32_t other = shim.Create();
  CHECK(lobby != other && shim.Owns(lobby) && !shim.Owns(0x1234));
  shim.Bind(lobby, 1000);
  shim.Bind(other, 1001);
  const auto routes = Routes({Peer("m1", 1, kHostCode, true), Peer("m1", 3, kThirdCode, false)});
  const uint8_t payload[] = {0x00, 0x03, 'h', 'i', '!'};
  CHECK(!shim.SendTo(lobby, routes[0].address, 1000, payload, sizeof payload, {}, link));
  CHECK(!shim.SendTo(lobby, 0xC0A80105, 1000, payload, sizeof payload, routes, link));
  CHECK(link.sent.empty());
  CHECK(shim.SendTo(lobby, routes[0].address, 1000, payload, sizeof payload, routes, link));
  CHECK(shim.SendTo(lobby, routes[1].address, 1000, payload, sizeof payload, routes, link));
  CHECK(link.sent.size() == 2 && link.sent[0].first == 1 && link.sent[1].first == 3);
  CHECK(link.sent[0].second.from_port == 1000 && link.sent[0].second.to_port == 1000);
  CHECK(link.sent[0].second.data == std::vector<uint8_t>(payload, payload + sizeof payload));

  link.inbox.push_back({1, {2000, 1001, {9, 9}}});
  link.inbox.push_back({3, {2000, 1000, {1, 2, 3}}});
  link.inbox.push_back({1, {2000, 4444, {7}}});
  link.inbox.push_back({4, {2000, 1000, {8}}});
  const auto got = shim.RecvFrom(lobby, routes, link);
  CHECK(got && got->from_address == routes[1].address && got->from_port == 2000);
  CHECK(got && got->data == (std::vector<uint8_t>{1, 2, 3}));
  CHECK(!shim.RecvFrom(lobby, routes, link));
  CHECK(shim.queued(other) == 1);
  const auto other_got = shim.RecvFrom(other, routes, link);
  CHECK(other_got && other_got->data == (std::vector<uint8_t>{9, 9}) && other_got->from_address == routes[0].address);
  CHECK(!shim.RecvFrom(lobby, {}, link));
}

void TestShimForgetsAStaleSession() {
  VdpShim shim;
  FakeLink link;
  const uint32_t socket = shim.Create();
  shim.Bind(socket, 1000);
  const auto first = Routes({Peer("m1", 1, kHostCode, true)});
  link.inbox.push_back({1, {1000, 1000, {1}}});
  link.inbox.push_back({1, {1000, 1000, {2}}});
  CHECK(shim.RecvFrom(socket, first, link).has_value());
  CHECK(shim.queued(socket) == 1);
  const auto second = Routes({Peer("m2", 1, kHostCode, true)});
  CHECK(!shim.RecvFrom(socket, second, link));
  CHECK(shim.queued(socket) == 0);
  shim.Close(socket);
  CHECK(!shim.Owns(socket));
  CHECK(!shim.SendTo(socket, second[0].address, 1000, nullptr, 0, second, link));
}

void TestLobbyDatagramFormat() {
  const np::LobbyDatagram d{1000, 1001, {1, 2, 3}};
  const std::vector<uint8_t> bytes = np::EncodeLobby(0xA1B2C3D4, d);
  CHECK(bytes.size() == np::kLobbyHeaderBytes + 3 && bytes[0] == 'T' && bytes[3] == 'B');
  const auto decoded = np::DecodeLobby(bytes.data(), bytes.size());
  CHECK(decoded && decoded->first == 0xA1B2C3D4 && decoded->second.from_port == 1000 &&
        decoded->second.to_port == 1001 && decoded->second.data == d.data);
  const uint8_t gameplay[] = {'S', 'F', 'N', 'P', 1, 2, 3, 4, 5, 6, 7, 8, 9};
  CHECK(!np::DecodeLobby(gameplay, sizeof gameplay));
  CHECK(!np::DecodeLobby(bytes.data(), 5));
}

void TestMuxCarriesLobbyAndGameplayOverLoopback() {
  np::UdpTransport a_socket, b_socket, c_socket;
  CHECK(a_socket.Open(0, true) && b_socket.Open(0, true) && c_socket.Open(0, true));
  np::LobbyMux a(a_socket), b(b_socket), c(c_socket);
  const uint32_t loopback = 0x7F000001;
  const uint32_t tag = SessionTag("m1");
  for (np::LobbyMux* mux : {&a, &b, &c}) mux->SetMatch(tag);
  a.SetPeer(2, {loopback, b_socket.bound_port()});
  a.SetPeer(3, {loopback, c_socket.bound_port()});
  b.SetPeer(1, {loopback, a_socket.bound_port()});
  c.SetPeer(1, {loopback, a_socket.bound_port()});
  CHECK(a.HasPeer(2) && a.HasPeer(3) && !a.HasPeer(4) && !a.SendLobby(4, {1000, 1000, {1}}));
  CHECK(a.SendLobby(2, {1000, 1000, {0, 2, 'o', 'k'}}));
  CHECK(a.SendLobby(3, {1000, 1000, {0, 3, 'h', 'i'}}));
  const uint8_t game[] = {'G', 'A', 'M', 'E'};
  CHECK(a.Send({loopback, b_socket.bound_port()}, game, sizeof game));
  np::LobbyDatagram got;
  int slot = 0;
  bool lobby_arrived = false;
  for (int i = 0; i < 200 && !lobby_arrived; ++i) lobby_arrived = b.TakeLobby(slot, got);
  CHECK(lobby_arrived && slot == 1 && got.data == (std::vector<uint8_t>{0, 2, 'o', 'k'}) && got.to_port == 1000);
  lobby_arrived = false;
  for (int i = 0; i < 200 && !lobby_arrived; ++i) lobby_arrived = c.TakeLobby(slot, got);
  CHECK(lobby_arrived && slot == 1 && got.data == (std::vector<uint8_t>{0, 3, 'h', 'i'}));
  uint8_t buffer[64];
  np::Endpoint from;
  size_t size = 0;
  for (int i = 0; i < 200 && !size; ++i) size = b.Receive(buffer, sizeof buffer, from);
  CHECK(size == sizeof game && std::memcmp(buffer, game, sizeof game) == 0 && from.port == a_socket.bound_port());
  CHECK(b.SendLobby(1, {1000, 1000, {5}}) && c.SendLobby(1, {1000, 1000, {6}}));
  int heard = 0;
  bool seen[5] = {};
  for (int i = 0; i < 400 && heard < 2; ++i) {
    if (a.TakeLobby(slot, got)) {
      ++heard;
      seen[slot] = true;
      CHECK(got.data == std::vector<uint8_t>{uint8_t(slot == 2 ? 5 : 6)});
    }
  }
  CHECK(heard == 2 && seen[2] && seen[3]);
  CHECK(a.lobby_sent() == 2 && a.lobby_received() == 2 && b.lobby_received() == 1);
  CHECK(a.lobby_received_from(2) == 1 && a.lobby_received_from(3) == 1 && a.lobby_received_from(4) == 0);

  np::UdpTransport stranger_socket;
  CHECK(stranger_socket.Open(0, true));
  np::LobbyMux stranger(stranger_socket);
  stranger.SetMatch(SessionTag("other"));
  stranger.SetPeer(1, {loopback, b_socket.bound_port()});
  CHECK(stranger.SendLobby(1, {1000, 1000, {6}}));
  stranger.SetMatch(tag);
  CHECK(stranger.SendLobby(1, {1000, 1000, {7}}));
  bool crossed = false;
  for (int i = 0; i < 100; ++i) crossed = crossed || b.TakeLobby(slot, got);
  CHECK(!crossed && b.lobby_rejected() >= 2);

  b.ClearPeer(1);
  CHECK(!b.HasPeer(1) && !b.SendLobby(1, {1000, 1000, {7}}));
  b.SetMatch(0);
  b.SetPeer(1, {loopback, a_socket.bound_port()});
  CHECK(!b.SendLobby(1, {1000, 1000, {7}}));
}

// A participant reached through the Worker relay is the synthetic endpoint of its slot, and only that slot's own.
void TestMuxAcceptsRelayedAndDirectPeersByTheirOwnEndpoint() {
  FakeTransport wire;
  np::LobbyMux mux(wire);
  const uint32_t tag = SessionTag("m1");
  mux.SetMatch(tag);
  const np::Endpoint direct{0x0A000002, 7002};
  mux.SetPeer(2, direct);
  mux.SetPeer(3, np::RelayPeerEndpoint(3));
  CHECK(mux.SendLobby(2, {1000, 1000, {1}}) && mux.SendLobby(3, {1000, 1000, {2}}));
  CHECK(wire.sent.size() == 2 && wire.sent[0].first == direct && wire.sent[1].first == np::RelayPeerEndpoint(3));

  const auto wrap = [&](uint8_t b) { return np::EncodeLobby(tag, {1000, 1000, {b}}); };
  wire.inbox.emplace_back(direct, wrap(10));
  wire.inbox.emplace_back(np::RelayPeerEndpoint(3), wrap(11));
  wire.inbox.emplace_back(np::RelayPeerEndpoint(4), wrap(12));
  wire.inbox.emplace_back(np::Endpoint{0x0A000009, 7002}, wrap(13));
  wire.inbox.emplace_back(np::RelayPeerEndpoint(2), wrap(14));
  np::LobbyDatagram got;
  int slot = 0;
  CHECK(mux.TakeLobby(slot, got) && slot == 2 && got.data[0] == 10);
  CHECK(mux.TakeLobby(slot, got) && slot == 3 && got.data[0] == 11);
  CHECK(mux.TakeLobby(slot, got) && slot == 2 && got.data[0] == 14);
  CHECK(!mux.TakeLobby(slot, got));
  CHECK(mux.lobby_rejected() == 2 && mux.lobby_received() == 3);
}

np::LobbyDatagram Datagram(uint8_t first, size_t size = 9) {
  np::LobbyDatagram datagram;
  datagram.from_port = 1000;
  datagram.to_port = 1000;
  datagram.data.assign(size, first);
  return datagram;
}

// The game's first lobby datagrams are sent before a pair's transport is selected; they wait for it instead of being lost.
void TestLobbyHoldKeepsEarlyDatagramsForTheirMatchAndSlotOnly() {
  np::LobbyHold hold;
  CHECK(!hold.Hold(0, 2, Datagram(1)));
  CHECK(!hold.Hold(7, 0, Datagram(1)) && !hold.Hold(7, 5, Datagram(1)));
  CHECK(!hold.Hold(7, 2, Datagram(1, np::kMaxLobbyPayload + 1)));
  CHECK(hold.Hold(7, 2, Datagram(1)) && hold.Hold(7, 2, Datagram(2)) && hold.size() == 2);
  const std::vector<np::LobbyDatagram> taken = hold.Take(7, 2);
  CHECK(taken.size() == 2 && taken[0].data[0] == 1 && taken[1].data[0] == 2 && hold.size() == 0);
  CHECK(hold.Take(7, 2).empty());

  // Each participant's datagrams wait for that participant's own transport.
  CHECK(hold.Hold(7, 2, Datagram(3)) && hold.Hold(7, 3, Datagram(4)) && hold.Hold(7, 4, Datagram(5)));
  const std::vector<np::LobbyDatagram> third = hold.Take(7, 3);
  CHECK(third.size() == 1 && third[0].data[0] == 4 && hold.size() == 2);
  CHECK(hold.Take(7, 3).empty() && hold.Take(7, 2).size() == 1 && hold.Take(7, 4).size() == 1 && hold.size() == 0);

  // Another match's datagrams are never delivered to this one.
  CHECK(hold.Hold(7, 2, Datagram(3)));
  CHECK(hold.Hold(8, 2, Datagram(4)) && hold.size() == 1);
  CHECK(hold.Take(7, 2).empty());
  CHECK(hold.Hold(9, 2, Datagram(5)));
  CHECK(hold.Take(8, 2).empty() && hold.size() == 0);

  // Bounded: a stalled transport cannot grow it.
  for (size_t i = 0; i < np::kMaxHeldLobby; ++i) CHECK(hold.Hold(9, 2, Datagram(uint8_t(i))));
  CHECK(!hold.Hold(9, 3, Datagram(0)) && hold.size() == np::kMaxHeldLobby);
  hold.Clear();
  CHECK(hold.size() == 0 && hold.Take(9, 2).empty());

  // Flushed through a mux, they reach the peer as the game sent them.
  np::UdpTransport sender, receiver;
  CHECK(sender.Open(58161, true) && receiver.Open(58162, true));
  np::LobbyMux mux(sender);
  mux.SetMatch(9);
  mux.SetPeer(3, np::Endpoint{0x7F000001, 58162});
  CHECK(hold.Hold(9, 3, Datagram(0x42)));
  for (const np::LobbyDatagram& held : hold.Take(9, 3)) CHECK(mux.SendLobby(3, held));
  uint8_t buffer[64];
  np::Endpoint from;
  size_t got = 0;
  for (int i = 0; i < 50 && got == 0; ++i) {
    got = receiver.Receive(buffer, sizeof buffer, from);
    if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto decoded = np::DecodeLobby(buffer, got);
  CHECK(decoded && decoded->first == 9 && decoded->second.data == Datagram(0x42).data);
}

void TestPortsAndEndpointsFollowTheAssignment() {
  CHECK(kGameplayPorts[0] == 7001 && kGameplayPorts[3] == 7004);
  online::PeerInfo peer;
  peer.ip = "203.0.113.9";
  peer.port = 7001;
  CHECK(online::ChoosePeerAddress({"192.168.1.5"}, peer) == "203.0.113.9");
  peer.same_network = true;
  peer.lan = {"127.0.0.1", "192.168.1.9"};
  CHECK(online::ChoosePeerAddress({"192.168.1.5"}, peer) == "192.168.1.9");
  CHECK(online::ChoosePeerAddress({"192.168.1.9"}, peer) == "127.0.0.1");
}

// A 64 KiB stand-in for guest memory: guest addresses are offsets into it.
struct GuestRam {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000, 0);
  uint8_t* base() { return bytes.data(); }
  void Buffers(uint32_t at, std::initializer_list<std::pair<uint32_t, uint32_t>> buffers) {
    uint32_t i = 0;
    for (const auto& [length, pointer] : buffers) {
      StoreBE32(base(), at + i * 8, length);
      StoreBE32(base(), at + i * 8 + 4, pointer);
      ++i;
    }
  }
};

void TestBuffersGatherAndScatterOneDatagram() {
  GuestRam ram;
  ram.Buffers(0x100, {{2, 0x200}, {3, 0x300}});
  ram.bytes[0x200] = 0x00;
  ram.bytes[0x201] = 0x03;
  std::memcpy(ram.base() + 0x300, "TMN", 3);
  CHECK((GatherBuffers(ram.base(), 0x100, 2) == std::vector<uint8_t>{0x00, 0x03, 'T', 'M', 'N'}));
  ram.Buffers(0x400, {{4, 0x500}, {8, 0x600}});
  CHECK(ScatterBuffers(ram.base(), 0x400, 2, {1, 2, 3, 4, 5, 6}) == 6);
  CHECK(ram.bytes[0x503] == 4 && ram.bytes[0x600] == 5 && ram.bytes[0x601] == 6 && ram.bytes[0x602] == 0);
  ram.Buffers(0x700, {{2, 0x800}});
  CHECK(ScatterBuffers(ram.base(), 0x700, 1, {9, 9, 9}) == 2);  // a short buffer keeps what fits, like a datagram
  ram.Buffers(0x900, {{0, 0}, {3, 0xA00}});
  CHECK((GatherBuffers(ram.base(), 0x900, 2) == std::vector<uint8_t>{0, 0, 0}));
}

void TestPendingReceivesCompleteOldestFirstPerSocket() {
  PendingReceives pending;
  CHECK(!pending.Take(1) && pending.size() == 0);
  pending.Add({1, 0x10, 1, 0, 0, 0, 0x100});
  pending.Add({2, 0x20, 1, 0, 0, 0, 0x200});
  pending.Add({1, 0x30, 1, 0, 0, 0, 0x300});
  CHECK(pending.Has(1) && pending.Has(2) && !pending.Has(3));
  CHECK((pending.handles() == std::vector<uint32_t>{1, 2}));
  CHECK(pending.Take(1)->overlapped == 0x100);
  CHECK(pending.Take(1)->overlapped == 0x300);
  CHECK(!pending.Take(1));
  pending.Drop(2);
  CHECK(pending.size() == 0);
}

void TestShimTakesTheTitlesDatagramSockets() {
  CHECK(ShimSocket(kSockDgram, kVdpProtocol) && ShimSocket(kSockDgram, 17) && ShimSocket(kSockDgram, 0));
  CHECK(!ShimSocket(1, 6) && !ShimSocket(1, kVdpProtocol));
  CHECK(kIoPending == 997 && kIoIncomplete == 996 && kWouldBlock == 10035 && kStatusPending == 0x103);
  CHECK(kOverlappedStatus == 0 && kOverlappedBytes == 4 && kOverlappedEvent == 16);
}

// The peer layer may bind a port the other console's datagrams do not name; its one socket still receives them.
void TestOnlySocketReceivesAnUnboundPort() {
  VdpShim shim;
  FakeLink link;
  const auto routes = Routes({Peer("m1", 1, kHostCode, true)});
  const uint32_t handle = shim.Create();
  shim.Bind(handle, 0);
  link.inbox.push_back({1, np::LobbyDatagram{9103, 9103, {1, 2, 3}}});
  const auto received = shim.RecvFrom(handle, routes, link);
  CHECK(received && received->data == std::vector<uint8_t>({1, 2, 3}) && received->from_port == 9103 &&
        received->from_address == routes[0].address);
  const uint32_t second = shim.Create();
  shim.Bind(second, 9200);
  link.inbox.push_back({1, np::LobbyDatagram{9103, 9103, {4}}});
  CHECK(!shim.RecvFrom(handle, routes, link) && !shim.RecvFrom(second, routes, link));
}

void TestSearchResultsDescribeTheFriendsSession() {
  online::PlayerMatchAd ad;
  ad.session = "0123456789abcdef";
  ad.player = kHostCode;
  std::vector<uint8_t> out(kSearchHeaderBytes + 2 * kSearchResultBytes, 0xCC);
  constexpr uint32_t kHeader = 0x40001000;
  CHECK(WriteSearchResults(out.data(), kHeader, uint32_t(out.size()), 20, {ad}) == 1);
  CHECK(LoadBE32(out.data(), 0) == 1 && LoadBE32(out.data(), 4) == kHeader + kSearchHeaderBytes);
  const uint8_t* result = out.data() + kSearchHeaderBytes;
  CHECK(SessionIdText(result) == ad.session);
  CHECK(XnAddrXuid(result + 8) == VirtualXuid(kHostCode));
  CHECK(LoadBE32(result, kSessionInfoBytes) == 1 && LoadBE32(result, kSessionInfoBytes + 8) == 1);
  CHECK(WriteSearchResults(out.data(), kHeader, kSearchHeaderBytes, 20, {ad}) == 0);
  CHECK((SessionIdBytes("not-hex-at-all!!") == std::vector<uint8_t>(8, 0)));
  CHECK(VirtualXuid(kHostCode) >> 48 == 0x0009 && VirtualXuid(kHostCode) != VirtualXuid(kJoinCode));
  CHECK(LiveAvailable(true, true) && !LiveAvailable(false, true) && !LiveAvailable(true, false));
}

void TestSearchByIdAnswersOneResultInTheSearchLayout() {
  online::PlayerMatchAd ad;
  ad.session = "0123456789abcdef";
  ad.player = kHostCode;
  constexpr uint32_t kHeader = 0x40002000;
  std::vector<uint8_t> out(kSearchByIdBytes, 0xCC);
  CHECK(WriteSearchByIdResult(out.data(), kHeader, uint32_t(out.size()), ad));
  CHECK(LoadBE32(out.data(), 0) == 1 && LoadBE32(out.data(), 4) == kHeader + kSearchHeaderBytes);
  const uint8_t* result = out.data() + kSearchHeaderBytes;
  CHECK(SessionIdText(result) == ad.session && XnAddrXuid(result + 8) == VirtualXuid(kHostCode));
  CHECK(out[kSearchByIdBytes - 1] == 0 && out[kSearchHeaderBytes + kSearchResultBytes] == 0);  // the rest is zeroed
  std::vector<uint8_t> small(kSearchHeaderBytes + kSearchResultBytes - 1, 0xCC);
  CHECK(!WriteSearchByIdResult(small.data(), kHeader, uint32_t(small.size()), ad));
  CHECK(kSearchByIdBytes == kSearchHeaderBytes + 1326);
}

void TestInviteInfoIsAnXInviteInfoOfTheHostsSession() {
  std::vector<uint8_t> info(kInviteInfoBytes, 0xCC);
  const uint64_t invitee = VirtualXuid(kJoinCode);
  const uint64_t inviter = VirtualXuid(kHostCode);
  WriteInviteInfo(info.data(), invitee, inviter, "0123456789abcdef");
  CHECK(LoadBE64(info.data(), 0) == invitee && LoadBE64(info.data(), 8) == inviter);
  CHECK(LoadBE32(info.data(), 16) == 0x584109D2 && kTmntTitleId == 0x584109D2);
  CHECK(SessionIdText(info.data() + 20) == "0123456789abcdef");
  CHECK(XnAddrXuid(info.data() + 20 + 8) == inviter);
  CHECK(LoadBE32(info.data(), 80) == 1 && kInviteInfoBytes == 84 && kInviteAcceptedNotification == 0x02000002);
  // A session id the title compares by value: the first byte is the most significant.
  const std::vector<uint8_t> id = SessionIdBytes("0123456789abcdef");
  CHECK(id[0] == 0x01 && id[7] == 0xEF);
}

void TestInviteHooksCoverTheTitlesInviteCalls() {
  const std::string hook = ReadSource("player_match_invite_hook.cpp");
  CHECK(Has(hook, "TMNT_INVITE_HOOK(823C76A8, InviteAcceptedInfo)") && Has(hook, "TMNT_INVITE_HOOK(8243CB30, SessionSearchById)"));
  CHECK(Has(hook, "BroadcastNotification(kInviteAcceptedNotification, OnlineUser())"));
  CHECK(Has(hook, "CompleteOverlappedImmediate(overlapped, 0)") && Has(hook, "ctx.r3.u64 = kInsufficientBuffer;"));
  CHECK(!Has(ReadSource("player_match_hook.cpp"), "8243CB30"));
}

void TestNetKeyIsASystemLinkIdAndAnExchangeKey() {
  uint8_t id_a[kNetKeyIdBytes], key_a[kNetKeyBytes], id_b[kNetKeyIdBytes], key_b[kNetKeyBytes], id_c[kNetKeyIdBytes],
      key_c[kNetKeyBytes];
  MakeNetKey(1, id_a, key_a);
  MakeNetKey(1, id_b, key_b);
  MakeNetKey(2, id_c, key_c);
  CHECK(std::memcmp(id_a, id_b, sizeof id_a) == 0 && std::memcmp(key_a, key_b, sizeof key_a) == 0);
  CHECK(std::memcmp(id_a, id_c, sizeof id_a) != 0 && std::memcmp(key_a, key_c, sizeof key_a) != 0);
  CHECK((id_a[0] & 0xF0) == 0 && (id_c[0] & 0xF0) == 0);
  uint8_t zero_id[kNetKeyIdBytes] = {}, zero_key[kNetKeyBytes] = {};
  CHECK(std::memcmp(id_a, zero_id, sizeof id_a) != 0 && std::memcmp(key_a, zero_key, sizeof key_a) != 0);
}

void TestDetailsOfAHostOnlySessionThenTwoAndFourMembers() {
  const auto write = [](SessionDetails& details, std::vector<uint8_t>& out, std::vector<uint8_t>& members) {
    out.assign(kDetailsBytes, 0xCC);
    members.assign(kMemberBytes * online::kMaxParty, 0xCC);
    WriteSessionDetails(out.data(), members.data(), 0x40003000, details);
  };
  SessionDetails details;
  details.max_public = 3;
  details.max_private = 1;
  details.state = kSessionStateLobby;
  details.session = "0123456789abcdef";
  details.members.push_back({VirtualXuid(kHostCode), 0});
  std::vector<uint8_t> out, members;
  write(details, out, members);
  CHECK(LoadBE32(out.data(), 16) == 3 && LoadBE32(out.data(), 20) == 1);
  CHECK(LoadBE32(out.data(), 24) == 3 && LoadBE32(out.data(), 28) == 0);  // the host takes the private slot
  CHECK(LoadBE32(out.data(), 32) == 1 && LoadBE32(out.data(), 36) == 1 && LoadBE32(out.data(), 40) == kSessionStateLobby);
  CHECK(LoadBE32(out.data(), 124) == 0x40003000 && LoadBE64(members.data(), 0) == VirtualXuid(kHostCode));
  CHECK(LoadBE32(members.data(), 8) == 0);
  CHECK(SessionIdText(out.data() + 56) == details.session);

  details.members.push_back({VirtualXuid(kJoinCode), kRemoteUserIndex});
  write(details, out, members);
  CHECK(LoadBE32(out.data(), 32) == 2 && LoadBE32(out.data(), 24) == 2 && LoadBE32(out.data(), 28) == 0);
  CHECK(LoadBE32(members.data(), kMemberBytes + 8) == kRemoteUserIndex);

  details.members.push_back({VirtualXuid("CCCC-4444"), kRemoteUserIndex});
  details.members.push_back({VirtualXuid("DDDD-5555"), kRemoteUserIndex});
  write(details, out, members);
  CHECK(LoadBE32(out.data(), 32) == 4 && LoadBE32(out.data(), 24) == 0 && LoadBE32(out.data(), 28) == 0);
  CHECK(LoadBE64(members.data(), 3 * kMemberBytes) == VirtualXuid("DDDD-5555"));

  CHECK(kDetailsHostOffset == 64 && kDetailsHostOffset + kXnAddrBytes <= 116);
  uint8_t xnaddr[kXnAddrBytes];
  WriteXnAddr(xnaddr, VirtualXuid(kHostCode));
  CHECK(XnAddrXuid(xnaddr) == VirtualXuid(kHostCode));
}

void TestShimReadinessAndConnectedPeer() {
  VdpShim shim;
  FakeLink link;
  const uint32_t socket = shim.Create();
  shim.Bind(socket, 1000);
  const auto routes = Routes({Peer("m1", 1, kHostCode, true), Peer("m1", 3, kThirdCode, false)});
  CHECK(!shim.Readable(socket, routes, link));
  CHECK(!shim.Readable(socket, {}, link));
  CHECK(!shim.Readable(0x1234, routes, link));
  link.inbox.push_back({3, {2000, 1000, {5, 6}}});
  CHECK(shim.Readable(socket, routes, link));
  CHECK(shim.Readable(socket, routes, link));
  const auto got = shim.RecvFrom(socket, routes, link);
  CHECK(got && got->data == (std::vector<uint8_t>{5, 6}));
  CHECK(!shim.Readable(socket, routes, link));

  uint32_t address = 0;
  uint16_t port = 0;
  CHECK(!shim.Peer(socket, address, port));
  shim.Connect(socket, routes[1].address, 2000);
  CHECK(shim.Peer(socket, address, port) && address == routes[1].address && port == 2000);
  CHECK(!shim.Peer(0x1234, address, port));
  shim.Close(socket);
  CHECK(!shim.Peer(socket, address, port));
}

void TestSocketHooksCoverSendRecvSelectConnect() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  for (const char* name : {"8243D120, Connect", "8243D138, Select", "8243D178, Recv", "8243D208, Send"}) {
    CHECK(Has(net, std::string("_HOOK(") + name + ")"));
  }
  const size_t select = net.find("void Select(");
  const std::string body = net.substr(select, net.find("\n}\n", select) - select);
  CHECK(Has(body, "g_shim.Readable(handle, routes, g_link)") && Has(body, "tmnt::SelectMixed(requested"));
  CHECK(Has(body, "original(ctx, base);"));
  CHECK(Has(body, "timeout.milliseconds = tmnt::OnFrameThread() ? 0 : seconds * 1000 + microseconds / 1000;"));
  CHECK(Has(body, "tmnt::NoteSelect(now() - select_started,"));
  CHECK(!Has(net, "SocketProbe"));
}

struct SelectHarness {
  std::set<uint32_t> shim = {0x5D500001, 0x5D500002};
  std::set<uint32_t> shim_readable;
  std::set<uint32_t> real_ready;
  int real_ready_after_polls = 0;
  int polls = 0;
  uint32_t clock = 0;
  bool real_fails = false;
  uint32_t shim_readable_at = 0xFFFFFFFFu;

  SelectOutcome Run(FdSets requested, SelectTimeout timeout) {
    return SelectMixed(
        requested, [&](uint32_t h) { return shim.count(h) != 0; },
        [&](uint32_t h) { return shim_readable.count(h) != 0 || clock >= shim_readable_at; },
        [&](FdSets& sets) {
          ++polls;
          if (real_fails) return -1;
          const bool ready = polls > real_ready_after_polls;
          FdSets out;
          int n = 0;
          for (uint32_t h : sets.read) {
            if (ready && real_ready.count(h)) { out.read.push_back(h); ++n; }
          }
          for (uint32_t h : sets.write) {
            if (ready && real_ready.count(h)) { out.write.push_back(h); ++n; }
          }
          for (uint32_t h : sets.except) {
            if (ready && real_ready.count(h)) { out.except.push_back(h); ++n; }
          }
          sets = out;
          return n;
        },
        timeout, [&] { return clock; }, [&] { clock += 1; });
  }
};

void TestMixedSelect() {
  constexpr uint32_t kShimA = 0x5D500001, kShimB = 0x5D500002, kRealA = 100, kRealB = 101;
  SelectHarness h;
  // shim only: readable and writable answered locally, nothing waits
  h.shim_readable = {kShimA};
  auto r = h.Run({{kShimA, kShimB}, {kShimB}, {}}, {false, 50});
  CHECK(r.count == 2 && r.ready.read == std::vector<uint32_t>{kShimA} && r.ready.write == std::vector<uint32_t>{kShimB});
  CHECK(h.polls == 0);
  // shim only, nothing readable: the caller's timeout passes, so a thread paced by select does not spin
  h = SelectHarness();
  r = h.Run({{kShimA}, {}, {}}, {false, 50});
  CHECK(r.count == 0 && r.ready.empty() && h.clock == 50 && h.polls == 0);
  // shim only, a datagram arrives during the wait: returned as soon as it is there
  h = SelectHarness();
  h.shim_readable_at = 7;
  r = h.Run({{kShimA}, {}, {}}, {false, 50});
  CHECK(r.count == 1 && r.ready.read == std::vector<uint32_t>{kShimA} && h.clock == 7);
  // shim only, zero timeout: an immediate poll
  h = SelectHarness();
  r = h.Run({{kShimA}, {}, {}}, {false, 0});
  CHECK(r.count == 0 && h.clock == 0);
  // shim only, writable: a datagram socket can always take one, and write readiness never waits
  h = SelectHarness();
  r = h.Run({{kShimA}, {kShimB}, {}}, {false, 50});
  CHECK(r.count == 1 && r.ready.read.empty() && r.ready.write == std::vector<uint32_t>{kShimB} && h.clock == 0);
  // shim only, infinite: never blocks the caller
  h = SelectHarness();
  r = h.Run({{kShimA}, {}, {}}, {true, 0});
  CHECK(r.count == 0 && h.clock == 0);
  // real only: forwarded, honours the finite timeout by repeating the zero-timeout poll
  h = SelectHarness();
  h.real_ready = {kRealA};
  h.real_ready_after_polls = 3;
  r = h.Run({{kRealA}, {}, {}}, {false, 10});
  CHECK(r.count == 1 && r.ready.read == std::vector<uint32_t>{kRealA} && h.polls == 4);
  // real only, never ready: stops at the timeout
  h = SelectHarness();
  r = h.Run({{kRealA}, {}, {}}, {false, 5});
  CHECK(r.count == 0 && h.clock >= 5 && h.polls >= 5);
  // zero timeout polls once
  h = SelectHarness();
  r = h.Run({{kRealA}, {}, {}}, {false, 0});
  CHECK(r.count == 0 && h.polls == 1);
  // mixed: shim readable, real not ready: returns at once with the shim socket
  h = SelectHarness();
  h.shim_readable = {kShimA};
  h.real_ready_after_polls = 100;
  r = h.Run({{kShimA, kRealA}, {}, {}}, {false, 1000});
  CHECK(r.count == 1 && r.ready.read == std::vector<uint32_t>{kShimA} && h.clock == 0);
  // mixed: shim not ready, real ready after a few polls: waits for the real one
  h = SelectHarness();
  h.real_ready = {kRealB};
  h.real_ready_after_polls = 2;
  r = h.Run({{kShimA, kRealB}, {}, {}}, {false, 1000});
  CHECK(r.count == 1 && r.ready.read == std::vector<uint32_t>{kRealB} && h.polls == 3);
  // mixed: both ready, one merged answer across the sets
  h = SelectHarness();
  h.shim_readable = {kShimA};
  h.real_ready = {kRealA, kRealB};
  r = h.Run({{kShimA, kRealA}, {kShimB, kRealB}, {}}, {false, 0});
  CHECK(r.count == 4);
  CHECK(r.ready.read == (std::vector<uint32_t>{kShimA, kRealA}));
  CHECK(r.ready.write == (std::vector<uint32_t>{kShimB, kRealB}));
  // except set: shim sockets are never exceptional, real ones pass through
  h = SelectHarness();
  h.real_ready = {kRealA};
  r = h.Run({{}, {}, {kShimA, kRealA}}, {false, 0});
  CHECK(r.count == 1 && r.ready.except == std::vector<uint32_t>{kRealA});
  // infinite wait with nothing ready never blocks the caller
  h = SelectHarness();
  r = h.Run({{kShimA, kRealA}, {}, {}}, {true, 0});
  CHECK(r.count == 0 && h.polls == 1);
  // a failing real select fails the call
  h = SelectHarness();
  h.real_fails = true;
  h.shim_readable = {kShimA};
  r = h.Run({{kShimA, kRealA}, {}, {}}, {false, 10});
  CHECK(r.count == -1);
}

// Delivers what is queued for it and nothing else, as the mux does.
class BurstLink final : public LobbyLink {
 public:
  explicit BurstLink(int count) : left_(count) {}
  bool Send(int, const np::LobbyDatagram&) override { return true; }
  bool Take(int& slot, np::LobbyDatagram& out) override {
    if (left_ == 0) return false;
    --left_;
    slot = 2;
    out = {};
    out.to_port = 9876;
    out.data = {1};
    return true;
  }

 private:
  int left_;
};

void TestShimQueuesStayBoundedAndCountTheirLosses() {
  VdpShim shim;
  const uint32_t handle = shim.Create();
  shim.Bind(handle, 9876);
  PeerRoute route;
  route.slot = 2;
  route.address = 0x0A530001;
  route.tag = 7;
  const std::vector<PeerRoute> routes = {route};
  BurstLink quiet(0);
  CHECK(!shim.Readable(handle, routes, quiet) && !shim.RecvFrom(handle, routes, quiet));
  CHECK(shim.deepest_queue() == 0 && shim.dropped() == 0);
  BurstLink one(1);
  CHECK(shim.Readable(handle, routes, one) && shim.Readable(handle, routes, quiet));
  CHECK(shim.RecvFrom(handle, routes, quiet).has_value());
  CHECK(!shim.Readable(handle, routes, quiet) && !shim.RecvFrom(handle, routes, quiet));
  BurstLink burst(300);
  CHECK(shim.Readable(handle, routes, burst));
  CHECK(shim.deepest_queue() == 128 && shim.dropped() == 172);
  int drained = 0;
  while (shim.RecvFrom(handle, routes, quiet)) ++drained;
  CHECK(drained == 128 && shim.deepest_queue() == 0 && !shim.Readable(handle, routes, quiet));
}

// A model of the title's UDP transport around one receive slot and the socket's hooks: the guest side mirrors
// sub_82490320, sub_82490158, sub_824A7CE0 and sub_824A6F78; the wait is the hook's WaitInSlices.
class NetZReceiveModel {
 public:
  static constexpr uint32_t kOverlapped = 0x40001000;
  static constexpr uint32_t kIdle = 0;
  static constexpr uint32_t kPending = 1;
  static constexpr uint32_t kDone = 2;

  void Arrive(std::vector<uint8_t> datagram) { queue_.push_back(std::move(datagram)); }

  void IssueReceive() {
    error_.Clear();
    if (TakeInto(sync_bytes_)) {
      event_ = false;  // sub_82490320 resets the event of a receive that finished at once
      slot_ = kDone;
      overlapped_done_ = true;
      return;
    }
    event_ = false;
    overlapped_done_ = false;
    tmnt::PendingReceive receive;
    receive.handle = 1;
    receive.overlapped = kOverlapped;
    pending_.Add(receive);
    error_.Set(tmnt::kIoPending);
    CHECK(error_.Get() == tmnt::kIoPending);
    slot_ = kPending;
    if (error_.Get() != tmnt::kIoPending) slot_ = kDone;
  }

  void Pump() {
    while (pending_.Has(1) && !queue_.empty()) {
      pending_.Take(1);
      TakeInto(sync_bytes_);
      overlapped_done_ = true;
      event_ = true;
    }
  }

  // sub_824A7CE0 with the title's 10 ms: true when the slot has a finished operation.
  bool Wait(const std::function<void(uint32_t)>& during_slice, uint32_t milliseconds = 10) {
    if (slot_ == kDone) return true;
    uint32_t clock = 0;
    const uint32_t status = WaitInSlices(
        milliseconds, [&] { Pump(); },
        [&](uint32_t slice) {
          ++slices_;
          if (event_) return 0u;
          during_slice(clock);
          clock += slice;
          return kWaitSliceTimedOut;
        },
        [&] { return clock; });
    waited_ms_ += clock;
    if (status == kWaitSliceTimedOut) return false;
    CHECK(status == 0);
    CHECK(event_);
    event_ = false;  // the 1-event poll that follows, then WSAResetEvent
    slot_ = kDone;
    return true;
  }

  // sub_824A6F78: the size is valid only when WSAGetOverlappedResult succeeds.
  uint32_t Consume() {
    CHECK(slot_ == kDone);
    error_.Clear();
    Pump();
    const uint32_t bytes = overlapped_done_ ? sync_bytes_ : 0;
    if (!overlapped_done_) error_.Set(tmnt::kIoIncomplete);
    slot_ = kIdle;
    return bytes;
  }

  bool event() const { return event_; }
  uint32_t slot() const { return slot_; }
  uint32_t slices() const { return slices_; }
  uint32_t waited_ms() const { return waited_ms_; }
  uint32_t last_error() const { return error_.Get(); }
  size_t pending() const { return pending_.size(); }

 private:
  bool TakeInto(uint32_t& bytes) {
    if (queue_.empty()) return false;
    bytes = uint32_t(queue_.front().size());
    queue_.pop_front();
    return true;
  }

  std::deque<std::vector<uint8_t>> queue_;
  tmnt::PendingReceives pending_;
  tmnt::SocketLastError error_;
  bool event_ = true;
  bool overlapped_done_ = false;
  uint32_t slot_ = kIdle;
  uint32_t sync_bytes_ = 0;
  uint32_t slices_ = 0;
  uint32_t waited_ms_ = 0;
};

void TestReceiveCompletedWhileWaiting() {
  for (uint32_t arrival : {0u, 5u}) {
    NetZReceiveModel net;
    net.IssueReceive();
    CHECK(net.slot() == NetZReceiveModel::kPending && net.last_error() == tmnt::kIoPending);
    CHECK(!net.event());
    CHECK(net.Wait([&](uint32_t clock) {
      if (clock == arrival) net.Arrive(std::vector<uint8_t>(42));
    }));
    CHECK(net.slot() == NetZReceiveModel::kDone && !net.event());
    CHECK(net.pending() == 0);
    CHECK(net.Consume() == 42);
    net.IssueReceive();
    CHECK(net.slot() == NetZReceiveModel::kPending && net.pending() == 1);
  }
}

void TestIdleReceiveBlocksThroughTheWholeWait() {
  NetZReceiveModel net;
  net.IssueReceive();
  CHECK(!net.Wait([](uint32_t) {}));
  CHECK(net.slot() == NetZReceiveModel::kPending);
  CHECK(net.waited_ms() == 10 && net.slices() == 3);
  CHECK(net.pending() == 1);
}

void TestDatagramQueuedBeforeTheReceiveFinishesAtOnce() {
  NetZReceiveModel net;
  net.Arrive(std::vector<uint8_t>(19));
  net.IssueReceive();
  CHECK(net.slot() == NetZReceiveModel::kDone && net.last_error() == 0 && !net.event());
  CHECK(net.Wait([](uint32_t) { CHECK(false); }));
  CHECK(net.waited_ms() == 0);
  CHECK(net.Consume() == 19);
}

void TestDatagramInTheLastSliceIsNotAFalseTimeout() {
  NetZReceiveModel net;
  net.IssueReceive();
  CHECK(net.Wait([&](uint32_t clock) {
    if (clock == 5) net.Arrive(std::vector<uint8_t>(7));
  }));
  CHECK(net.Consume() == 7);
}

void TestReceiveStartLeavesItsEventClear() {
  NetZReceiveModel net;
  CHECK(net.event());
  net.IssueReceive();
  CHECK(!net.event());
  const std::string hook = ReadSource("player_match_net_hook.cpp");
  CHECK(Has(hook, "ClearEvent(LoadBE32(GuestHost(base, receive.overlapped), tmnt::kOverlappedEvent));"));
}

void TestANewReceiveReplacesAnAbandonedOneOnTheSameOverlapped() {
  tmnt::PendingReceives pending;
  tmnt::PendingReceive first;
  first.handle = 1;
  first.overlapped = 0x100;
  first.buffers = 0x200;
  tmnt::PendingReceive second = first;
  second.buffers = 0x300;
  pending.Add(first);
  pending.Add(second);
  CHECK(pending.size() == 1 && pending.Take(1)->buffers == 0x300);
  tmnt::PendingReceive other = first;
  other.overlapped = 0x180;
  pending.Add(first);
  pending.Add(other);
  CHECK(pending.size() == 2);
}

void TestLastErrorLeftAloneByNonSocketCalls() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  CHECK(Has(net, "TMNT_NET_HOOK(8243D2C0, WsaWaitForEvents)"));
  CHECK(Has(net, "TMNT_NET_HOOK(823CB410, XnAddrToInAddr)"));
  CHECK(Has(net, "TMNT_SOCKET_HOOK(8243D1B0, WsaRecvFrom)") && Has(net, "TMNT_SOCKET_HOOK(8243D158, GetOverlappedResult)"));
}

constexpr uint32_t kSlotPending = 1;
constexpr uint32_t kSlotDone = 2;

// sub_82490320 reads the error once, then sub_82490158 reads it again to pick the slot state.
uint32_t GuestSlotAfterFailedReceive(const tmnt::SocketLastError& error) {
  if (error.Get() != tmnt::kIoPending) return kSlotDone;
  return error.Get() == tmnt::kIoPending ? kSlotPending : kSlotDone;
}

void TestAPendingReceiveStaysPendingForBothGuestReads() {
  tmnt::SocketLastError error;
  error.Set(tmnt::kIoPending);
  CHECK(GuestSlotAfterFailedReceive(error) == kSlotPending);
  CHECK(error.Get() == tmnt::kIoPending);
  error.Clear();
  CHECK(error.Get() == 0);
  CHECK(GuestSlotAfterFailedReceive(error) == kSlotDone);
  error.Set(tmnt::kWouldBlock);
  CHECK(GuestSlotAfterFailedReceive(error) == kSlotDone);

  const std::string net = ReadSource("player_match_net_hook.cpp");
  const size_t hook = net.find("void LastError(");
  const std::string body = net.substr(hook, net.find("\n}", hook) - hook);
  CHECK(Has(body, "g_last_error.Get()") && !Has(body, "Clear") && !Has(body, "Set("));
  CHECK(Has(net, "g_last_error.Clear();"));
}

void TestFramePhasesReachTheHeartbeat() {
  const std::string bridge = ReadSource("online_bridge.cpp");
  for (const char* field : {"tick_ms+{}", "pace_ms+{}", "swap_ms+{}", "frame_cpu_ms+{}", "process_cpu_ms+{}", "select_calls+{}",
                            "select_read+{}", "select_write+{}", "select_except+{}", "shim_queue_max={}", "shim_dropped+{}",
                            "stages_ms={}"}) {
    CHECK(Has(bridge, field));
  }
  CHECK(Has(ReadSource("window_host.cpp"), "tmnt::NotePresentPhases("));
  CHECK(Has(ReadSource("frame_trace.cpp"), "tmnt::TickScope tick;"));
  CHECK(Has(ReadSource("frame_trace.cpp"), "tmnt::StageScope scope(0x##addr, slot);"));
  CHECK(Has(ReadSource("frame_trace.cpp"), "tmnt::NoteFrameThreadForSampler(&ctx, base);"));
  CHECK(Has(bridge, "DEBUG_SAULO frame block {}"));
  CHECK(Has(bridge, "SetFrameSamplerActive(game - last_game < kSlowTicksPerSecond);"));
}

void TestTheSamplerOnlyRunsWhileTicksAreSlowAndNeverLogsWhileSuspended() {
  const std::string sampler = ReadSource("frame_block_sampler.cpp");
  CHECK(Has(sampler, "if (!g_active.load()) continue;"));
  CHECK(Has(sampler, "if (!FrameSamplerEnabled() ||"));
  const size_t suspended = sampler.find("SuspendThread(thread)");
  const size_t resumed = sampler.find("ResumeThread(thread)");
  CHECK(suspended != std::string::npos && resumed != std::string::npos && suspended < resumed);
  const std::string held = sampler.substr(suspended, resumed - suspended);
  CHECK(!Has(held, "Count(") && !Has(held, "fmt::") && !Has(held, "std::string"));
}

void TestTheGameRunsItsOwnNetworking() {
  for (const char* source : {"online_bridge.cpp", "player_match_net_hook.cpp", "player_match_hook.cpp", "online_runtime.cpp",
                             "netplay/online_game_socket.cpp", "netplay/lobby_mux.cpp"}) {
    const std::string text = ReadSource(source);
    for (const char* banned : {"RollbackCore", "GuestSimulation", "SaveSnapshot", "LoadSnapshot", "Resimulate"}) {
      CHECK(!Has(text, banned));
    }
  }
}

void TestWaitSlices() {
  CHECK(NextWaitSlice(kInfiniteWait, 0) == 5 && NextWaitSlice(kInfiniteWait, 100000) == 5);
  CHECK(NextWaitSlice(0, 0) == 0 && NextWaitSlice(3, 0) == 3 && NextWaitSlice(100, 0) == 5);
  CHECK(NextWaitSlice(100, 98) == 2 && NextWaitSlice(100, 100) == 0 && NextWaitSlice(100, 500) == 0);
  uint32_t waited = 0;
  uint32_t slices = 0;
  const uint32_t status = WaitInSlices(
      23, [] {},
      [&](uint32_t slice) {
        waited += slice;
        ++slices;
        return kWaitSliceTimedOut;
      },
      [&] { return waited; });
  CHECK(status == kWaitSliceTimedOut && waited == 23 && slices == 6);
}

void TestOverlappedAndSessionHookFixes() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  const size_t wsa = net.find("void WsaRecvFrom(");
  const std::string recv = net.substr(wsa, net.find("\n}\n", wsa) - wsa);
  CHECK(Has(recv, "receive.bytes = 0;\n  g_pending.Add(receive);"));
  CHECK(Has(recv, "if (receive.overlapped) SignalEvent("));
  const size_t close = net.find("void Close(");
  const std::string closer = net.substr(close, net.find("\n}\n", close) - close);
  CHECK(Has(closer, "kStatusCancelled") && Has(closer, "g_pending.Take(handle)") && Has(closer, "SignalEvent("));
  const size_t result = net.find("void GetOverlappedResult(");
  const std::string polled = net.substr(result, net.find("\n}\n", result) - result);
  CHECK(polled.find("tmnt::CompleteVdpReceives(base);") < polled.find("kStatusPending"));
  const std::string live = ReadSource("player_match_hook.cpp");
  const size_t create = live.find("void SessionCreate(");
  const std::string created = live.substr(create, live.find("\n}\n", create) - create);
  CHECK(created.find("ctx.r5.u32") < created.find("original(ctx, base);"));
  CHECK(Has(created, "const uint32_t public_slots = ctx.r5.u32;") && !Has(created, "original(ctx, base);\n  if (!live) return;\n  if (flags & tmnt::kSessionCreateHost) {\n    const uint32_t public_slots"));
  CHECK(Has(ReadSource("player_match.h"), "user == kAnyUser"));
}

// PLT_IsSignedIntoFullAccountOnline as the title computes it: state 2 (sub_822FD0F0) and no guest flag (sub_822FD120).
constexpr bool TitleSeesFullAccountOnline(uint32_t state, uint32_t info_flags) {
  constexpr uint32_t kGuestFlag = 0x2;
  return state == kSignedInToLive && !(info_flags & kGuestFlag);
}

void TestOnlyTheOnlineUserIsOnLive() {
  CHECK(LiveAvailable(true, true) && !LiveAvailable(true, false) && !LiveAvailable(false, true) && !LiveAvailable(false, false));
  for (uint32_t online_user = 0; online_user < 4; ++online_user) {
    for (uint32_t user = 0; user < 4; ++user) {
      CHECK(LiveProfileUser(user, online_user, true) == (user == online_user));
      CHECK(LivePrivilegeUser(user, online_user, true) == (user == online_user));
      CHECK(!LiveProfileUser(user, online_user, false) && !LivePrivilegeUser(user, online_user, false));
    }
    CHECK(!LiveProfileUser(kAnyUser, online_user, true) && LivePrivilegeUser(kAnyUser, online_user, true));
    CHECK(!LivePrivilegeUser(kAnyUser, online_user, false));
  }
  CHECK(!LocalUser(4) && !LocalUser(uint32_t(-1)));
}

void TestSigninInfoIsTheLiveProfile() {
  const uint64_t xuid = VirtualXuid(kHostCode);
  std::vector<uint8_t> info(kSigninInfoBytes, 0xCC);
  WriteSigninInfo(info.data(), xuid, "Saulo-b");
  CHECK(LoadBE64(info.data(), 0) == xuid);
  CHECK(LoadBE32(info.data(), 8) == kSigninInfoOnlineEnabled && LoadBE32(info.data(), 12) == kSignedInToLive);
  CHECK(std::string(reinterpret_cast<const char*>(info.data() + 24)) == "Saulo-b");
  CHECK(TitleSeesFullAccountOnline(LoadBE32(info.data(), 12), LoadBE32(info.data(), 8)));
  WriteSigninInfo(info.data(), xuid, "ABCDEFGHIJKLMNOP");
  CHECK(std::string(reinterpret_cast<const char*>(info.data() + 24)) == "ABCDEFGHIJKLMNO");
  // The offline answers the title gets without the service: its own LOC_ERR_XLA_NOT_SIGNED_IN.
  CHECK(!TitleSeesFullAccountOnline(1, kSigninInfoOnlineEnabled) && !TitleSeesFullAccountOnline(0, 0));
  CHECK(!TitleSeesFullAccountOnline(kSignedInToLive, 0x2));
  CHECK(VirtualXuid(kHostCode) == xuid && VirtualXuid(kJoinCode) != xuid);
}

uint32_t ImageWord(const std::string& image, uint32_t offset) {
  return offset + 4 <= image.size() ? LoadBE32(reinterpret_cast<const uint8_t*>(image.data()), offset) : 0;
}

bool ImageRangeHas(const std::string& image, uint32_t base, uint32_t address, uint32_t bytes, uint32_t word, uint32_t mask) {
  for (uint32_t at = address; at < address + bytes; at += 4) {
    if ((ImageWord(image, at - base) & mask) == word) return true;
  }
  return false;
}

// The addresses behind the sign-in gate, against the game's own code; skipped when the extracted images are absent.
void TestTheOnlineGateIsTheTitlesOwn() {
  const std::string analysis = std::string(TMNT_SOURCE_DIR) + "/game/analysis/";
  const std::string exe = ReadPath(analysis + "default.image.bin");
  const std::string dll = ReadPath(analysis + "tmnt_r.image.bin");
  if (exe.empty() || dll.empty()) {
    std::printf("  SKIP online gate images not found\n");
  } else {
    constexpr uint32_t kExeBase = 0x82000000;
    constexpr uint32_t kDllBase = 0x87500000;
    constexpr uint32_t kBranchAndLink = 0x48000001;
    const uint32_t state_call = ImageWord(exe, 0x822FD0F0 - kExeBase + 0x0C);
    CHECK((state_call & 0xFC000003) == kBranchAndLink);
    CHECK(0x822FD0FC + (uint32_t(int32_t(state_call << 6) >> 6) & ~3u) == 0x826EDD50);
    constexpr uint32_t kLiRegister196 = 0x380000C4;
    CHECK(ImageRangeHas(dll, kDllBase, 0x875D4DF0, 0x370, kLiRegister196, 0xFC1FFFFF));
    CHECK(!ImageRangeHas(dll, kDllBase, 0x875D3AA0, 0x370, kLiRegister196, 0xFC1FFFFF));
  }
  const std::string gate = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(Has(gate, "REX_HOOK_RAW(sub_875D4DF0)"));
  CHECK(Has(gate, "rex::cvar::SetFlagByName(\"tmnt_online_user\", std::to_string(controller));"));
  const std::string hook = ReadSource("player_match_hook.cpp");
  CHECK(Has(hook, "REXCVAR_DEFINE_INT32(tmnt_online_user, 0,"));
  CHECK(Has(hook, "tmnt::LiveProfileUser(user, online_user, CurrentLive().available)"));
  CHECK(Has(hook, "tmnt::LivePrivilegeUser(user, CurrentOnlineUser(), CurrentLive().available)"));
  CHECK(Has(hook, "tmnt::WriteSigninInfo(GuestHost(base, info), live.xuid, live.name);"));
  CHECK(Has(hook, "if (!tmnt::OnlineRunning()) return live;"));
  CHECK(Has(hook, "live.available = tmnt::LiveAvailable(true, profile.signed_in) && !profile.friend_code.empty();"));
  CHECK(!Has(hook, "kOnlineUser") && !Has(ReadSource("player_match_invite_hook.cpp"), "kOnlineUser"));
}

void TestHooksCoverTheTitlesWrappers() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  for (const char* name : {"823CB410, XnAddrToInAddr", "823CB450, QosListen", "823CB470, QosLookup",
                           "8243D090, Socket", "8243D0A8, Close", "8243D108, Bind", "8243D158, GetOverlappedResult",
                           "8243D190, RecvFrom", "8243D1B0, WsaRecvFrom", "8243D220, SendTo", "8243D240, WsaSendTo",
                           "8243D2C0, WsaWaitForEvents"}) {
    CHECK(Has(net, std::string("_HOOK(") + name + ")"));
  }
  CHECK(Has(net, "REX_HOOK_RAW(sub_8243D2A0) { LastError(ctx, base, __imp__sub_8243D2A0); }"));
  CHECK(Has(net, "!tmnt::ShimSocket(type, protocol) || !LiveNow()"));
  for (const char* name : {"823CB3E0, XNetCreateKey", "823CB3F0, XNetKeyRegistration", "823CB400, XNetKeyRegistration"}) {
    CHECK(Has(net, std::string("_HOOK(") + name + ")"));
  }
  CHECK(Has(net, "if (!id || !key || !LiveNow())") && Has(net, "ctx.r3.u64 = 0;"));
  const std::string live = ReadSource("player_match_hook.cpp");
  for (const char* name : {"826EDD50, SigninState", "826EDD60, CheckPrivilege", "826EE0C8, SigninInfo",
                           "826EE078, GetXuid", "826EDD48, GetName", "823CB4E0, TitleXnAddr",
                           "8243C480, SessionCreate", "8243CA48, SessionSearchEx", "8243C850, SessionLeaveRemote",
                           "8243C6F0, SessionDelete", "823CB4D0, QosRelease"}) {
    CHECK(Has(live, std::string("TMNT_LIVE_HOOK(") + name + ")"));
  }
  const size_t details = live.find("void SessionGetDetails(");
  const size_t details_end = live.find("\n}\n", details);
  const std::string body = live.substr(details, details_end - details);
  CHECK(Has(body, "size < tmnt::kDetailsBytes") && Has(body, "CompleteOverlappedImmediate(overlapped, 0)") &&
        Has(body, "ctx.r3.u64 = tmnt::kIoPending;"));
  CHECK(body.find("original(ctx, base);") < body.find("WriteSessionDetails"));
  CHECK(kIoPending == 997 && kDetailsBytes == 128);
  for (const std::string& text : {net, live}) CHECK(!Has(text, "token"));
  // Online play off: nothing is answered differently and the service is never created.
  CHECK(Has(ReadSource("online_bridge.cpp"), "if (!OnlineRunning()) return;"));
  CHECK(Has(live, "if (!tmnt::OnlineRunning()) return live;"));
  CHECK(Has(net, "bool LiveNow() { return tmnt::OnlineRunning() && tmnt::Online().Profile().signed_in; }"));
  CHECK(Has(ReadSource("window_host.cpp"), "CountGuestFrame();\n  tmnt::PumpOnlineNetwork(base);"));
}


void TestJoinedSessionSlotsAreTheAdvertisedPlayerCount() {
  online::PlayerMatchAd ad;
  ad.session = "0123456789abcdef";
  ad.player = kHostCode;
  for (const int slots : {2, 3, 4}) {
    ad.slots = slots;
    std::vector<uint8_t> out(kSearchHeaderBytes + kSearchResultBytes, 0);
    CHECK(WriteSearchResults(out.data(), 0x40003000, uint32_t(out.size()), 1, {ad}) == 1);
    const SessionSlots joined = SearchResultSlots(out.data() + kSearchHeaderBytes);
    CHECK(joined.max_public == uint32_t(slots) && joined.max_private == 0 && joined.Total() == uint32_t(slots));
  }
  std::vector<uint8_t> raw(kSearchResultBytes, 0);
  StoreBE32(raw.data(), kSessionInfoBytes + 0, 1);
  StoreBE32(raw.data(), kSessionInfoBytes + 4, 2);
  StoreBE32(raw.data(), kSessionInfoBytes + 8, 3);
  StoreBE32(raw.data(), kSessionInfoBytes + 12, 4);
  const SessionSlots mixed = SearchResultSlots(raw.data());
  CHECK(mixed.max_public == 4 && mixed.max_private == 6 && mixed.Total() == 10);
  const std::string live = ReadSource("player_match_hook.cpp");
  const std::string dll = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(Has(live, "tmnt::SearchResultSlots(GuestHost(base, info))") && Has(live, "PublishJoinedSlots(slots.Total())"));
  CHECK(Has(live, "PublishJoinedSlots(0);\n    }\n    if (info) tmnt::WriteXnAddr"));
  CHECK(Has(dll, "REX_HOOK_RAW(sub_875CCEC8)") && Has(dll, "tmnt_online_joined_slots") && Has(dll, "if (slots > 0) ctx.r3.u64"));
  CHECK(dll.find("__imp__sub_875CCEC8(ctx, base);") < dll.find("tmnt_online_joined_slots"));
}

// Two members can hash to one address. The test forces it: only the two low bytes of an XUID choose the address.
void TestAddressCollisionsResolveTheSameOnEveryConsole() {
  const uint64_t base = 0x0009000000004142ull;
  const uint64_t host = base | 0x1111ull << 32, two = base | 0x2222ull << 32, three = base | 0x3333ull << 32,
                 four = base | 0x4444ull << 32;
  CHECK(SyntheticAddress(host) == SyntheticAddress(two) && SyntheticAddress(two) == SyntheticAddress(four));

  SyntheticAddressBook first, second;
  first.Update({{1, host}, {2, two}, {3, three}, {4, four}});
  second.Update({{4, four}, {3, three}, {1, host}, {2, two}});  // another console's order
  const uint32_t a = first.AddressOf(host), b = first.AddressOf(two), c = first.AddressOf(three), d = first.AddressOf(four);
  CHECK(a == SyntheticAddress(host));  // the address the host's search result shows
  CHECK(a != b && a != c && a != d && b != c && b != d && c != d);
  CHECK(second.AddressOf(host) == a && second.AddressOf(two) == b && second.AddressOf(three) == c && second.AddressOf(four) == d);
  for (const uint32_t address : {a, b, c, d}) {
    CHECK(IsSyntheticAddress(address));
    CHECK((address & 0xFF) != 0 && (address & 0xFF) != 255 && ((address >> 8) & 0xFF) != 0 && ((address >> 8) & 0xFF) != 255);
  }

  // A member keeps its address for the whole session, even when the member it collided with leaves.
  first.Update({{1, host}, {3, three}, {4, four}});
  CHECK(first.AddressOf(three) == c && first.AddressOf(four) == d);
  CHECK(first.AddressOf(two) == SyntheticAddress(two));  // cleaned on leave
  first.Update({{1, host}, {2, two}, {3, three}, {4, four}});
  CHECK(first.AddressOf(two) == b);  // and the free address is the one a later member takes
  first.Update({});
  CHECK(first.AddressOf(three) == SyntheticAddress(three));

  // The last address of the block wraps to the first, never to a reserved one.
  const uint64_t last = 0x000900000000FDFDull, also_last = 0x000900010000FDFDull;
  CHECK((SyntheticAddress(last) & 0xFFFF) == 0xFEFE);
  SyntheticAddressBook wrap;
  wrap.Update({{1, last}, {2, also_last}});
  CHECK((wrap.AddressOf(also_last) & 0xFFFF) == 0x0101);

  // The game reaches a moved member through its XNADDR, and both directions of the route use the moved address.
  uint8_t xnaddr[kXnAddrBytes];
  WriteXnAddr(xnaddr, two, b);
  CHECK(XnAddrXuid(xnaddr) == two && LoadBE32(xnaddr, 0) == b);
  SyntheticAddressBook book;
  book.Update({{1, host}, {2, two}});
  online::ActivePeer peer = Peer("m1", 2, "", false);
  peer.player = kJoinCode;
  CHECK(RouteFor(peer, book)->address == SyntheticAddress(VirtualXuid(kJoinCode)));  // a member the book does not list
  const std::vector<PeerRoute> routes = RoutesFor({Peer("m1", 1, kHostCode, true), Peer("m1", 3, kThirdCode, false)}, 2,
                                                  VirtualXuid(kJoinCode), book);
  CHECK(routes.size() == 2 && routes[0].address == book.AddressOf(VirtualXuid(kHostCode)) &&
        routes[1].address == book.AddressOf(VirtualXuid(kThirdCode)));
  const std::string hook = ReadSource("player_match_net_hook.cpp");
  CHECK(Has(hook, "tmnt::ActiveSyntheticAddress(*xuid)") && !Has(hook, "tmnt::SyntheticAddress(*xuid)"));
  CHECK(Has(ReadSource("player_match_hook.cpp"), "tmnt::WriteXnAddr(GuestHost(base, xnaddr), live.xuid, tmnt::ActiveSyntheticAddress(live.xuid));"));
}

// PLT_IsPlayerMuted: nobody is muted, friend or not.
void TestMuteListQueryAnswersNotMuted() {
  const std::string live = ReadSource("player_match_hook.cpp");
  const size_t query = live.find("void MuteListQuery(");
  const std::string body = live.substr(query, live.find("\n}\n", query) - query);
  CHECK(Has(live, "TMNT_LIVE_HOOK(823C7F20, MuteListQuery)") && !Has(live, "UsersAreFriends"));
  CHECK(Has(body, "StoreBE32(GuestHost(base, ctx.r5.u32), 0, 0);") && Has(body, "ctx.r3.u64 = 0;"));
  CHECK(!Has(body, "original(ctx, base)") && !Has(body, "Online()"));
}

void TestInviteLookupUsesTheSessionsRealSlots() {
  online::PlayerMatchAd ad;
  ad.session = "0123456789abcdef";
  ad.player = kHostCode;
  for (const int slots : {2, 3, 4}) {
    ad.slots = slots;
    std::vector<uint8_t> out(kSearchByIdBytes, 0);
    CHECK(WriteSearchByIdResult(out.data(), 0x40002000, uint32_t(out.size()), ad));
    CHECK(SearchResultSlots(out.data() + kSearchHeaderBytes).Total() == uint32_t(slots));
  }
  CHECK(Has(ReadSource("player_match_invite_hook.cpp"), "ad.slots = invite->slots;"));
  const std::string live = ReadSource("player_match_hook.cpp");
  CHECK(Has(live, "slots = {uint32_t(ad.slots), 0};"));
  CHECK(live.find("slots = {uint32_t(ad.slots), 0};") < live.find("PublishJoinedSlots(slots.Total());"));
}

}  // namespace

void TestJoinRefusalsReachTheTitlesOwnDialogs() {
  CHECK(JoinRefusalResult("") == 0);
  CHECK(JoinRefusalResult("party_full") == 0x80155202);
  for (const char* code : {"not_found", "offline", "host_left", "expired"}) CHECK(JoinRefusalResult(code) == 0x80155200);
  for (const char* code : {"link_lost", "limit", "rate_limited"}) CHECK(JoinRefusalResult(code) == 0x80151100);
  CHECK(JoinRefusalResult("version_mismatch") == 0x80004005);
  CHECK(JoinRefusalResult("busy") == 0x80004005);
  const std::string live = ReadSource("player_match_hook.cpp");
  CHECK(Has(live, "CompleteJoinWhenAnswered(started, overlapped)"));
  CHECK(Has(live, "EndOnlineMatch(\"join_refused\")"));
  CHECK(live.find("void EndOnlineMatch(") < live.find("void CompleteJoinWhenAnswered("));
}

int main() {
  TestJoinRefusalsReachTheTitlesOwnDialogs();
  TestActiveMatchCreatesARoute();
  TestXnAddrMapsBothWays();
  TestQosReturnsTheAdvertisedPeer();
  TestShimRoutesOnlyToTheParty();
  TestShimForgetsAStaleSession();
  TestLobbyDatagramFormat();
  TestMuxCarriesLobbyAndGameplayOverLoopback();
  TestMuxAcceptsRelayedAndDirectPeersByTheirOwnEndpoint();
  TestLobbyHoldKeepsEarlyDatagramsForTheirMatchAndSlotOnly();
  TestPortsAndEndpointsFollowTheAssignment();
  TestBuffersGatherAndScatterOneDatagram();
  TestPendingReceivesCompleteOldestFirstPerSocket();
  TestShimTakesTheTitlesDatagramSockets();
  TestShimReadinessAndConnectedPeer();
  TestMixedSelect();
  TestWaitSlices();
  TestOverlappedAndSessionHookFixes();
  TestJoinedSessionSlotsAreTheAdvertisedPlayerCount();
  TestAddressCollisionsResolveTheSameOnEveryConsole();
  TestMuteListQueryAnswersNotMuted();
  TestInviteLookupUsesTheSessionsRealSlots();
  TestSocketHooksCoverSendRecvSelectConnect();
  TestOnlySocketReceivesAnUnboundPort();
  TestSearchResultsDescribeTheFriendsSession();
  TestSearchByIdAnswersOneResultInTheSearchLayout();
  TestInviteInfoIsAnXInviteInfoOfTheHostsSession();
  TestInviteHooksCoverTheTitlesInviteCalls();
  TestNetKeyIsASystemLinkIdAndAnExchangeKey();
  TestDetailsOfAHostOnlySessionThenTwoAndFourMembers();
  TestHooksCoverTheTitlesWrappers();
  TestShimQueuesStayBoundedAndCountTheirLosses();
  TestAPendingReceiveStaysPendingForBothGuestReads();
  TestReceiveCompletedWhileWaiting();
  TestIdleReceiveBlocksThroughTheWholeWait();
  TestDatagramQueuedBeforeTheReceiveFinishesAtOnce();
  TestDatagramInTheLastSliceIsNotAFalseTimeout();
  TestReceiveStartLeavesItsEventClear();
  TestANewReceiveReplacesAnAbandonedOneOnTheSameOverlapped();
  TestLastErrorLeftAloneByNonSocketCalls();
  TestFramePhasesReachTheHeartbeat();
  TestTheSamplerOnlyRunsWhileTicksAreSlowAndNeverLogsWhileSuspended();
  TestTheGameRunsItsOwnNetworking();
  TestOnlyTheOnlineUserIsOnLive();
  TestSigninInfoIsTheLiveProfile();
  TestTheOnlineGateIsTheTitlesOwn();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
