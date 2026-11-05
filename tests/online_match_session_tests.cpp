// MatchSession: one match's network establishment per remote participant (STUN, direct punch, relay fallback), driven with
// scripted time, a real OnlineGameSocket on 127.0.0.1 (port 0) and raw loopback sockets standing in for the peers.
// Run: tmnt_online_match_session_tests

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "netplay/online_game_socket.h"
#include "netplay/relay_frame.h"
#include "netplay/udp_transport.h"
#include "online/match_session.h"
#include "online/rendezvous.h"
#include "online/telemetry.h"

using namespace tmnt::online;
using tmnt::netplay::Endpoint;
using tmnt::netplay::OnlineGameSocket;
using tmnt::netplay::UdpTransport;

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

constexpr uint32_t kLoopback = 0x7F000001;
constexpr uint32_t kToken = 3141592653u;
constexpr uint32_t kGeneration = 7;
constexpr auto npos = std::string::npos;

std::vector<std::string> g_scan;
std::vector<std::string> g_reasons;
std::vector<std::pair<std::string, std::string>> g_event_fields;

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct Options {
  int slot = 1;
  bool mesh = false;
  bool relay = true;
  bool forwards = true;
  bool open = true;
  bool sync = true;
  std::vector<Endpoint> stun;
  std::vector<std::string> lan{"192.168.1.5"};
  std::string stun_text = "stun.test:3478";
  std::function<std::vector<Endpoint>(const std::string&)> resolve;
};

Options Opts(int slot) {
  Options o;
  o.slot = slot;
  return o;
}

std::string EventText(const SessionEvent& event) {
  std::string text = event.name + " slot=" + std::to_string(event.slot);
  for (const auto& [key, value] : event.fields) text += " " + key + "=" + value;
  return text;
}

struct Harness {
  OnlineGameSocket socket;
  std::vector<std::string> texts, logs, notices;
  std::vector<SessionEvent> events;
  std::vector<std::vector<uint8_t>> frames;
  std::string stun_arg;
  std::unique_ptr<MatchSession> session;

  explicit Harness(const Options& o) {
    if (o.open) socket.Open({0}, true);
    MatchSessionConfig c;
    c.match = "match1";
    c.diag = "0123456789ab";
    c.my_slot = o.slot;
    c.relay_token = o.relay ? kToken : 0;
    c.generation = kGeneration;
    c.socket = &socket;
    c.lan = o.lan;
    c.stun = o.stun_text;
    if (o.resolve) {
      c.resolve_stun = o.resolve;
    } else {
      c.resolve_stun = [this, servers = o.stun](const std::string& stun) {
        stun_arg = stun;
        return servers;
      };
    }
    c.synchronous_probes = o.sync;
    c.service_forwards_public = o.forwards;
    c.full_mesh = o.mesh;
    c.log = [this](const std::string& line) {
      logs.push_back(line);
      g_scan.push_back(line);
    };
    c.send_text = [this](std::string&& text) { texts.push_back(std::move(text)); };
    c.send_binary = [this](std::vector<uint8_t>&& frame) {
      frames.push_back(std::move(frame));
      return true;
    };
    c.notice = [this](const std::string& text) {
      notices.push_back(text);
      g_scan.push_back(text);
    };
    c.event = [this](const SessionEvent& event) {
      events.push_back(event);
      g_scan.push_back(EventText(event));
      for (const auto& [key, value] : event.fields) {
        g_event_fields.emplace_back(key, value);
        if (key == "reason") g_reasons.push_back(value);
      }
    };
    session = std::make_unique<MatchSession>(std::move(c));
  }

  Endpoint SessionEndpoint() const { return {kLoopback, socket.bound_port()}; }

  std::vector<std::string> Ops(const std::string& op) const {
    std::vector<std::string> out;
    for (const std::string& text : texts) {
      if (text.find("\"op\":\"" + op + "\"") != npos) out.push_back(text);
    }
    return out;
  }
  bool Requested(int peer, const std::string& transport) const {
    for (const std::string& text : Ops("match.transport")) {
      if (text.find("\"peer\":" + std::to_string(peer) + ",") != npos &&
          text.find("\"transport\":\"" + transport + "\"") != npos) {
        return true;
      }
    }
    return false;
  }
  const SessionEvent* Event(const std::string& name, int slot) const {
    for (const SessionEvent& event : events) {
      if (event.name == name && event.slot == slot) return &event;
    }
    return nullptr;
  }
  int EventCount(const std::string& name, int slot) const {
    return int(std::count_if(events.begin(), events.end(),
                             [&](const SessionEvent& e) { return e.name == name && e.slot == slot; }));
  }
  static std::string Field(const SessionEvent* event, const std::string& key) {
    if (!event) return {};
    for (const auto& [k, v] : event->fields) {
      if (k == key) return v;
    }
    return {};
  }
  bool LogHas(const std::string& part) const {
    return std::any_of(logs.begin(), logs.end(), [&](const std::string& l) { return l.find(part) != npos; });
  }
  bool NoticeHas(const std::string& part) const {
    return std::any_of(notices.begin(), notices.end(), [&](const std::string& l) { return l.find(part) != npos; });
  }
};

struct Remote {
  UdpTransport sock;
  Remote() { sock.Open(0, true); }
  Endpoint ep() const { return {kLoopback, sock.bound_port()}; }
};

bool Recv(UdpTransport& sock, std::vector<uint8_t>& data, int wait_ms = 1000) {
  uint8_t buffer[2048];
  Endpoint from;
  for (int i = 0; i <= wait_ms / 5; ++i) {
    if (const size_t n = sock.Receive(buffer, sizeof buffer, from)) {
      data.assign(buffer, buffer + n);
      return true;
    }
    Sleep(5);
  }
  return false;
}

std::optional<PunchPacket> RecvPunch(Remote& remote, std::optional<PunchKind> kind = std::nullopt, int wait_ms = 1000) {
  for (int i = 0; i < 100; ++i) {
    std::vector<uint8_t> data;
    if (!Recv(remote.sock, data, wait_ms)) return std::nullopt;
    if (const auto packet = DecodePunch(data.data(), data.size()); packet && (!kind || packet->kind == *kind)) return packet;
  }
  return std::nullopt;
}

void SendPunch(Remote& remote, const Harness& h, PunchKind kind, uint8_t side, uint64_t key) {
  const std::vector<uint8_t> bytes = EncodePunch({kind, side, key});
  remote.sock.Send(h.SessionEndpoint(), bytes.data(), bytes.size());
}

template <class Done>
bool Until(Harness& h, double now, Done done) {
  for (int i = 0; i < 400; ++i) {
    h.session->Update(now);
    if (done()) return true;
    Sleep(5);
  }
  return false;
}

std::vector<RoomMember> Members(std::initializer_list<int> slots, const std::string& state = "connected") {
  std::vector<RoomMember> out;
  for (const int slot : slots) {
    RoomMember member;
    member.slot = slot;
    member.name = "P" + std::to_string(slot);
    member.state = state;
    out.push_back(member);
  }
  return out;
}

PeerInfo Info(int slot, const Endpoint& ep, bool forwards = true) {
  PeerInfo peer;
  peer.slot = slot;
  peer.name = "P" + std::to_string(slot);
  peer.ip = "127.0.0.1";
  peer.port = ep.port;
  peer.public_ip = "127.0.0.1";
  peer.public_port = ep.port;
  peer.service_forwards_public = forwards;
  return peer;
}

PeerInfo InfoWithoutPublic(int slot, const Endpoint& ep, bool forwards) {
  PeerInfo peer = Info(slot, ep, forwards);
  peer.public_ip.clear();
  peer.public_port = 0;
  return peer;
}

std::vector<uint8_t> StunAnswer(const std::vector<uint8_t>& request, const Endpoint& mapped) {
  std::vector<uint8_t> reply(32);
  reply[0] = 0x01, reply[1] = 0x01, reply[3] = 0x0c;
  std::memcpy(reply.data() + 4, request.data() + 4, 16);
  const uint16_t port = mapped.port ^ 0x2112;
  const uint32_t ip = mapped.address ^ 0x2112a442u;
  const uint8_t attribute[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(port >> 8), uint8_t(port),
                                 uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
  std::memcpy(reply.data() + 20, attribute, sizeof attribute);
  return reply;
}

bool SameAssignment(const std::optional<NetplayAssignment>& a, const std::optional<NetplayAssignment>& b) {
  if (!a || !b) return !a && !b;
  return a->slot == b->slot && a->peer_ip == b->peer_ip && a->peer_port == b->peer_port && a->generation == b->generation &&
         a->relay == b->relay;
}

std::string Reason(std::string_view code) { return std::string(code); }

int LogCount(const Harness& h, const std::string& part) {
  return int(std::count_if(h.logs.begin(), h.logs.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; }));
}

void RelayPair(Harness& h, Remote& peer, int slot) {
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, slot}), 100);
  s.Update(100);
  s.OnPeer(Info(slot, peer.ep()), 100.1);
  s.Update(102.6);
  s.OnTransport(1, slot, "relay", 1, 102.7);
  CHECK(s.peer(slot)->transport == MatchTransport::kRelay);
}

void TestARelayedPairWaitsForTheService() {
  {
    Harness h(Opts(1));
    Remote p2;
    RelayPair(h, p2, 2);
    MatchSession& s = *h.session;
    const auto assigned = s.Assignment(2);
    s.OnRelayClosed(0, "service_disconnected", 103);
    s.OnRelayClosed(2, "unavailable", 104);
    s.Update(132.9);
    CHECK(s.peer(2)->failure.empty() && SameAssignment(s.Assignment(2), assigned));
    s.OnMembers(Members({1, 2}), 133);  // the room is back, and the peer in it
    s.Update(200);
    CHECK(s.peer(2)->failure.empty() && s.peer(2)->transport == MatchTransport::kRelay);
    CHECK(LogCount(h, "relay suspended peer=2 reason=service_disconnected") == 1 && LogCount(h, "relay resumed peer=2") == 1);
  }
  {  // A peer that is still away neither resumes the pair nor outlives its seat.
    Harness h(Opts(1));
    Remote p2;
    RelayPair(h, p2, 2);
    MatchSession& s = *h.session;
    s.OnPeerLost(2);
    s.OnRelayClosed(2, "peer_disconnected", 103);
    std::vector<RoomMember> away = Members({1});
    const std::vector<RoomMember> lost = Members({2}, "disconnected");
    away.insert(away.end(), lost.begin(), lost.end());
    s.OnMembers(away, 104);
    s.Update(132.9);
    CHECK(s.peer(2)->failure.empty());
    s.Update(133.1);
    CHECK(s.peer(2)->failure == Reason(reason::relay_closed) && s.peer(2)->relay_result == "failed:relay_closed");
    s.Update(134);
    CHECK(LogCount(h, "transport lost peer=2") == 1);
  }
  {  // The host's pair ends a guest only when the wait ends.
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    s.OnTransport(1, 2, "relay", 1, 101);
    CHECK(s.peer(1)->transport == MatchTransport::kRelay);
    s.OnRelayClosed(1, "peer_unavailable", 102);
    s.Update(131.9);
    CHECK(!s.fatal());
    s.Update(132.1);
    CHECK(s.fatal());
  }
  {  // A relay the service closed on purpose does not come back.
    Harness h(Opts(1));
    Remote p2;
    RelayPair(h, p2, 2);
    h.session->OnRelayClosed(2, "peer_backpressure", 103);
    CHECK(h.session->peer(2)->failure == Reason(reason::relay_closed));
    h.session->OnMembers(Members({1, 2}), 104);
    CHECK(h.session->peer(2)->failure == Reason(reason::relay_closed));
  }
}

void TestStunAndEndpointAdvertisement() {
  Remote stun_server;
  Options o = Opts(1);
  o.stun = {stun_server.ep()};
  o.lan = {"192.168.1.5", "10.0.0.7"};
  Harness h(o);
  MatchSession& s = *h.session;
  CHECK(h.stun_arg == "stun.test:3478");
  s.ResendEndpoint();
  CHECK(h.Ops("match.endpoint").empty());
  s.OnMembers(Members({1, 2}), 100);
  s.Update(100);
  std::vector<uint8_t> request;
  CHECK(Recv(stun_server.sock, request) && request.size() == 20 && request[0] == 0x00 && request[1] == 0x01);
  CHECK(h.Ops("match.endpoint").empty());
  if (request.size() == 20) {
    const std::vector<uint8_t> answer = StunAnswer(request, {0x08080404, 40000});
    stun_server.sock.Send(h.SessionEndpoint(), answer.data(), answer.size());
  }
  CHECK(Until(h, 100.2, [&] { return !h.Ops("match.endpoint").empty(); }));
  CHECK(h.Ops("match.endpoint").size() == 1);
  const std::string text = h.Ops("match.endpoint").empty() ? std::string() : h.Ops("match.endpoint")[0];
  CHECK(text.find("\"port\":" + std::to_string(h.socket.bound_port())) != npos);
  CHECK(text.find("\"lan\":[\"192.168.1.5\",\"10.0.0.7\"]") != npos);
  CHECK(text.find("\"public_ip\":\"8.8.4.4\",\"public_port\":40000") != npos);
  s.Update(100.5);
  s.Update(101);
  CHECK(h.Ops("match.endpoint").size() == 1);
  s.ResendEndpoint();
  CHECK(h.Ops("match.endpoint").size() == 2 && h.Ops("match.endpoint")[1].find("\"public_port\":40000") != npos);
}

void TestStunThatNeverAnswersStillAdvertises() {
  Remote stun_server;
  Options o = Opts(1);
  o.stun = {stun_server.ep()};
  Harness h(o);
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  s.Update(100);
  s.Update(101);
  CHECK(h.Ops("match.endpoint").empty());
  s.Update(102.6);
  CHECK(h.Ops("match.endpoint").size() == 1);
  CHECK(h.Ops("match.endpoint")[0].find("public_") == npos);
  s.Update(103);
  CHECK(h.Ops("match.endpoint").size() == 1);
}

std::atomic<int> g_resolve_calls{0};

void TestAsynchronousStunLookup() {
  Options o = Opts(1);
  o.sync = false;
  o.resolve = [](const std::string& stun) {
    ++g_resolve_calls;
    return stun == "async.test:3478" ? std::vector<Endpoint>() : std::vector<Endpoint>{{kLoopback, 9}};
  };
  o.stun_text = "async.test:3478";
  Harness h(o);
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  CHECK(Until(h, 100, [&] { return !h.Ops("match.endpoint").empty(); }));
  CHECK(h.Ops("match.endpoint").size() == 1);
  CHECK(g_resolve_calls.load() == 1);
  CHECK(s.peer(2) && s.peer(2)->transport == MatchTransport::kDirectProbing);
}

void TestHostFallsBackToTheRelay() {
  Harness h(Opts(1));
  Remote p2;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  CHECK(s.peer(2) && s.peer(2)->transport == MatchTransport::kDirectProbing && s.peer(2)->chooser);
  CHECK(s.peer(2) && s.peer(2)->relay_available && s.peer(2)->direct_result == "none" && s.peer(2)->relay_result == "none");
  CHECK(h.Ops("match.endpoint").empty());
  s.Update(100);
  CHECK(h.Ops("match.endpoint").size() == 1);
  const std::string endpoint = h.Ops("match.endpoint").empty() ? std::string() : h.Ops("match.endpoint")[0];
  CHECK(endpoint.find("\"port\":" + std::to_string(h.socket.bound_port())) != npos);
  CHECK(endpoint.find("\"lan\":[\"192.168.1.5\"]") != npos && endpoint.find("public_") == npos);

  s.OnPeer(Info(2, p2.ep()), 100.1);
  const auto punch = RecvPunch(p2);
  CHECK(punch && punch->kind == PunchKind::kPunch && punch->side == 1 && punch->key == PairPunchKey("match1", 1, 2));

  s.OnTransport(1, 2, "relay", 1, 101);
  CHECK(s.peer(2)->transport == MatchTransport::kDirectProbing && !s.Assignment(2) && h.Ops("match.transport").empty());
  s.Update(102.4);
  CHECK(h.Ops("match.transport").empty() && s.peer(2)->fallback_reason.empty());
  s.Update(102.6);
  CHECK(h.Ops("match.transport").size() == 1 && h.Requested(2, "relay") && !h.Requested(2, "direct"));
  s.Update(103);
  s.Update(110);
  CHECK(h.Ops("match.transport").size() == 1);

  PeerView view = *s.peer(2);
  CHECK(view.fallback_reason == Reason(reason::direct_timeout));
  CHECK(view.direct_result == "failed:" + Reason(reason::direct_timeout));
  CHECK(view.transport == MatchTransport::kDirectProbing && !view.relay_used && view.relay_result == "none");
  CHECK(view.relay_available && view.failure.empty() && !s.Assignment(2));
  CHECK(h.EventCount("relay_fallback", 2) == 1 && Harness::Field(h.Event("relay_fallback", 2), "reason") == "direct_timeout");
  CHECK(Harness::Field(h.Event("direct_result", 2), "result") == "failed");

  s.OnTransport(1, 2, "relay", 1, 110.5);
  view = *s.peer(2);
  CHECK(view.transport == MatchTransport::kRelay && view.relay_used && view.relay_result == "ok");
  CHECK(view.fallback_reason == Reason(reason::direct_timeout) && view.establishment_ms >= 0 && view.failure.empty());
  const auto assignment = s.Assignment(2);
  CHECK(assignment && assignment->relay && assignment->slot == 2 && assignment->generation == kGeneration);
  CHECK(assignment && assignment->peer_ip == "127.0.0.242" && assignment->peer_port == 1);
  CHECK(s.Assignments().size() == 1);
  CHECK(h.socket.relay_selected(2));
  CHECK(h.NoticeHas("Connected with P2."));
  CHECK(h.EventCount("transport_selected", 2) == 1 && Harness::Field(h.Event("transport_selected", 2), "transport") == "relay");
  CHECK(Harness::Field(h.Event("relay_result", 2), "result") == "ok");
  s.OnTransport(1, 2, "relay", 1, 111);
  CHECK(h.EventCount("transport_selected", 2) == 1 && h.Ops("match.transport").size() == 1);
  s.Update(112);
  CHECK(s.peer(2)->transport == MatchTransport::kRelay && !s.fatal());
}

void TestDirectSuccessSelectsDirect() {
  Harness h(Opts(1));
  Remote p2;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  s.Update(100);
  s.OnPeer(Info(2, p2.ep()), 100.1);
  const uint64_t key = PairPunchKey("match1", 1, 2);
  CHECK(RecvPunch(p2, PunchKind::kPunch));

  SendPunch(p2, h, PunchKind::kPunch, 2, key);
  Until(h, 100.5, [] { return false; });
  const auto ack = RecvPunch(p2, PunchKind::kAck);
  CHECK(ack && ack->side == 1 && ack->key == key);
  CHECK(s.peer(2)->transport == MatchTransport::kDirectProbing);

  SendPunch(p2, h, PunchKind::kAck, 2, key);
  CHECK(Until(h, 101, [&] { return s.peer(2)->transport == MatchTransport::kDirect; }));
  const PeerView view = *s.peer(2);
  CHECK(view.direct_result == "ok" && view.fallback_reason.empty() && view.relay_result == "none" && !view.relay_used);
  CHECK(view.failure.empty() && view.establishment_ms >= 0);
  CHECK(h.Ops("match.transport").size() == 1 && h.Requested(2, "direct") && !h.Requested(2, "relay"));
  const auto assignment = s.Assignment(2);
  CHECK(assignment && !assignment->relay && assignment->slot == 2 && assignment->generation == kGeneration);
  CHECK(assignment && assignment->peer_ip == "127.0.0.1" && assignment->peer_port == p2.ep().port);
  CHECK(!h.socket.relay_selected(2));
  CHECK(Harness::Field(h.Event("direct_result", 2), "result") == "ok");
  CHECK(Harness::Field(h.Event("transport_selected", 2), "transport") == "direct");
  CHECK(h.EventCount("relay_fallback", 2) == 0);

  s.Update(110);
  s.OnTransport(1, 2, "relay", 1, 110.5);
  CHECK(s.peer(2)->transport == MatchTransport::kDirect && s.Assignment(2) && !s.Assignment(2)->relay);
  CHECK(h.Ops("match.transport").size() == 1);
  CHECK(h.NoticeHas("Connected with P2."));
}

void TestServiceThatForwardsNoPublicEndpoints() {
  {
    Options o = Opts(1);
    o.forwards = false;
    Harness h(o);
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    CHECK(h.Ops("match.transport").empty());
    s.Update(100);
    CHECK(h.Ops("match.transport").size() == 1 && h.Requested(2, "relay"));
    CHECK(s.peer(2)->fallback_reason == Reason(reason::service_public_endpoints_unsupported));
    CHECK(s.peer(2)->direct_result == "failed:" + Reason(reason::service_public_endpoints_unsupported));
    s.OnPeer(InfoWithoutPublic(2, p2.ep(), false), 100.1);
    CHECK(h.Ops("match.transport").size() == 1);
    std::vector<uint8_t> data;
    CHECK(!Recv(p2.sock, data, 100));
    s.OnTransport(1, 2, "relay", 1, 100.3);
    CHECK(s.peer(2)->transport == MatchTransport::kRelay && s.peer(2)->relay_result == "ok");
    CHECK(s.Assignment(2) && s.Assignment(2)->relay);
    CHECK(s.peer(2)->fallback_reason == Reason(reason::service_public_endpoints_unsupported));
  }
  {
    Harness h(Opts(1));
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    CHECK(h.Ops("match.transport").empty());
    s.OnPeer(InfoWithoutPublic(2, p2.ep(), false), 100.1);
    CHECK(h.Ops("match.transport").size() == 1 && h.Requested(2, "relay"));
    CHECK(s.peer(2)->fallback_reason == Reason(reason::service_public_endpoints_unsupported));
    s.Update(101);
    s.OnPeer(InfoWithoutPublic(2, p2.ep(), false), 101.1);
    s.Update(103);
    CHECK(h.Ops("match.transport").size() == 1);
    std::vector<uint8_t> data;
    CHECK(!Recv(p2.sock, data, 100));
  }
  {
    Options o = Opts(1);
    o.relay = false;
    Harness h(o);
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    CHECK(!s.peer(2)->relay_available);
    s.OnPeer(InfoWithoutPublic(2, p2.ep(), false), 100.1);
    const PeerView view = *s.peer(2);
    CHECK(view.transport == MatchTransport::kFailed && view.failure == Reason(reason::service_public_endpoints_unsupported));
    CHECK(view.direct_result == "failed:" + Reason(reason::service_public_endpoints_unsupported));
    CHECK(h.Ops("match.transport").empty() && !s.Assignment(2) && !s.fatal());
    CHECK(h.NoticeHas("out of date"));
  }
  {
    Harness h(Opts(1));
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.OnPeer(InfoWithoutPublic(2, p2.ep(), false), 100.05);
    s.Update(100.1);
    CHECK(h.Ops("match.transport").size() == 1 && h.Requested(2, "relay"));
    CHECK(s.peer(2)->fallback_reason == Reason(reason::service_public_endpoints_unsupported));
  }
}

struct Outcome {
  MatchTransport transport = MatchTransport::kPending;
  std::string fallback, direct, failure;
  size_t transport_requests = 0;
  bool relay_requested = false;
  bool direct_requested = false;
  bool operator==(const Outcome&) const = default;
};

Outcome Run(bool endpoint_first, const Options& options, const PeerInfo& info, double settle,
            const std::function<void(Harness&, MatchSession&)>& after_start = {}) {
  Harness h(options);
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  if (endpoint_first) {
    s.OnPeer(info, 100);
    s.Update(100);
  } else {
    s.Update(100);
    s.OnPeer(info, 100);
  }
  s.Update(100.1);
  if (after_start) after_start(h, s);
  s.Update(settle);
  const PeerView view = *s.peer(2);
  Outcome out;
  out.transport = view.transport;
  out.fallback = view.fallback_reason;
  out.direct = view.direct_result;
  out.failure = view.failure;
  out.transport_requests = h.Ops("match.transport").size();
  out.relay_requested = h.Requested(2, "relay");
  out.direct_requested = h.Requested(2, "direct");
  return out;
}

void TestEndpointAndStunOrderDoNotChangeTheOutcome() {
  Remote p2;
  const Endpoint ep = p2.ep();

  {
    const PeerInfo info = InfoWithoutPublic(2, ep, false);
    const Outcome first = Run(true, Opts(1), info, 100.2);
    const Outcome second = Run(false, Opts(1), info, 100.2);
    CHECK(first == second);
    CHECK(first.relay_requested && first.transport_requests == 1);
    CHECK(first.fallback == Reason(reason::service_public_endpoints_unsupported));
  }
  {
    Options no_relay = Opts(1);
    no_relay.relay = false;
    const PeerInfo info = InfoWithoutPublic(2, ep, false);
    const Outcome first = Run(true, no_relay, info, 100.2);
    const Outcome second = Run(false, no_relay, info, 100.2);
    CHECK(first == second);
    CHECK(first.transport == MatchTransport::kFailed && first.failure == Reason(reason::service_public_endpoints_unsupported));
    CHECK(first.transport_requests == 0);
  }
  {
    PeerInfo info = InfoWithoutPublic(2, ep, true);
    info.same_network = false;
    const Outcome first = Run(true, Opts(1), info, 102.7);
    const Outcome second = Run(false, Opts(1), info, 102.7);
    CHECK(first == second);
    CHECK(first.relay_requested && first.transport_requests == 1);
    CHECK(first.fallback == Reason(reason::stun_failed) || first.fallback == Reason(reason::no_public_endpoint));
    CHECK(first.direct == "failed:" + first.fallback);
  }
  {
    const Outcome first = Run(true, Opts(1), Info(2, ep), 104);
    const Outcome second = Run(false, Opts(1), Info(2, ep), 104);
    CHECK(first == second);
    CHECK(first.relay_requested && first.fallback == Reason(reason::direct_timeout));
  }
  {
    Options guest = Opts(2);
    const PeerInfo info = InfoWithoutPublic(1, ep, false);
    for (const bool endpoint_first : {true, false}) {
      Harness h(guest);
      MatchSession& s = *h.session;
      s.OnMembers(Members({1, 2}), 100);
      if (endpoint_first) {
        s.OnPeer(info, 100);
        s.Update(100);
      } else {
        s.Update(100);
        s.OnPeer(info, 100);
      }
      s.Update(105);
      CHECK(h.Ops("match.transport").empty());
      CHECK(s.peer(1)->transport == MatchTransport::kDirectProbing || s.peer(1)->transport == MatchTransport::kFailed);
    }
  }
}

void TestDirectLateSuccessIsTheSameInEitherOrder() {
  for (const bool endpoint_first : {true, false}) {
    Harness h(Opts(1));
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    if (endpoint_first) {
      s.OnPeer(Info(2, p2.ep()), 100);
      s.Update(100);
    } else {
      s.Update(100);
      s.OnPeer(Info(2, p2.ep()), 100);
    }
    const uint64_t key = PairPunchKey("match1", 1, 2);
    CHECK(RecvPunch(p2, PunchKind::kPunch));
    SendPunch(p2, h, PunchKind::kAck, 2, key);
    CHECK(Until(h, 102.3, [&] { return s.peer(2)->transport == MatchTransport::kDirect; }));
    CHECK(h.Requested(2, "direct") && !h.Requested(2, "relay") && s.peer(2)->fallback_reason.empty());
    CHECK(s.peer(2)->direct_result == "ok");
  }
}

void TestNonChooserFollowsTheChooser() {
  {
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    const auto punch = RecvPunch(p1);
    CHECK(punch && punch->side == 2 && punch->key == PairPunchKey("match1", 1, 2));
    CHECK(s.peer(1) && !s.peer(1)->chooser);
    s.Update(103);
    s.Update(106);
    s.Update(112);
    CHECK(h.Ops("match.transport").empty());
    CHECK(s.peer(1)->transport == MatchTransport::kDirectProbing && s.peer(1)->fallback_reason.empty());
    CHECK(!s.Assignment(1) && !s.fatal());
    s.OnTransport(1, 2, "relay", 1, 112.5);
    const PeerView view = *s.peer(1);
    CHECK(view.transport == MatchTransport::kRelay && view.relay_used && view.relay_result == "ok");
    CHECK(view.fallback_reason == "chooser_chose_relay" && view.direct_result == "failed:chooser_chose_relay");
    CHECK(s.Assignment(1) && s.Assignment(1)->relay && s.Assignment(1)->peer_ip == "127.0.0.241");
    CHECK(h.Ops("match.transport").empty());
  }
  {
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    CHECK(RecvPunch(p1, PunchKind::kPunch));
    SendPunch(p1, h, PunchKind::kPunch, 1, PairPunchKey("match1", 1, 2));
    Until(h, 101, [] { return false; });
    const auto ack = RecvPunch(p1, PunchKind::kAck);
    CHECK(ack && ack->side == 2);
    s.OnTransport(1, 2, "direct", 1, 101.5);
    const PeerView view = *s.peer(1);
    CHECK(view.transport == MatchTransport::kDirect && view.direct_result == "ok" && !view.relay_used);
    const auto assignment = s.Assignment(1);
    CHECK(assignment && !assignment->relay && assignment->peer_ip == "127.0.0.1" && assignment->peer_port == p1.ep().port);
    CHECK(h.Ops("match.transport").empty());
    CHECK(!s.fatal());
  }
  {
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    s.OnTransport(1, 2, "direct", 1, 101);
    const PeerView view = *s.peer(1);
    CHECK(view.transport == MatchTransport::kFailed && view.failure == Reason(reason::direct_handshake_failed));
    CHECK(!s.Assignment(1) && s.fatal());
    CHECK(h.Ops("match.transport").empty());
  }
  {
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    s.OnTransport(3, 4, "relay", 3, 101);
    s.OnTransport(1, 3, "relay", 1, 101);
    CHECK(s.peer(1)->transport == MatchTransport::kDirectProbing && !s.Assignment(1) && !s.fatal());
  }
}

void TestPairsAreIndependent() {
  Options o = Opts(2);
  o.mesh = true;
  Harness h(o);
  Remote p1, p3, p4;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2, 3, 4}), 100);
  s.Update(100);
  CHECK(s.peers().size() == 3 && s.peer(1) && s.peer(3) && s.peer(4));
  CHECK(!s.peer(1)->chooser && s.peer(3)->chooser && s.peer(4)->chooser);
  s.OnPeer(Info(1, p1.ep()), 100.1);
  s.OnPeer(Info(3, p3.ep()), 100.1);
  s.OnPeer(Info(4, p4.ep()), 100.1);
  const auto punch3 = RecvPunch(p3);
  const auto punch4 = RecvPunch(p4);
  const auto punch1 = RecvPunch(p1);
  CHECK(punch3 && punch3->side == 1 && punch3->key == PairPunchKey("match1", 2, 3));
  CHECK(punch4 && punch4->side == 1 && punch4->key == PairPunchKey("match1", 2, 4));
  CHECK(punch1 && punch1->side == 2 && punch1->key == PairPunchKey("match1", 1, 2));

  SendPunch(p4, h, PunchKind::kAck, 2, PairPunchKey("match1", 2, 4));
  CHECK(Until(h, 101, [&] { return s.peer(4)->transport == MatchTransport::kDirect; }));
  CHECK(s.peer(3)->transport == MatchTransport::kDirectProbing && s.peer(1)->transport == MatchTransport::kDirectProbing);
  CHECK(h.Requested(4, "direct") && h.Ops("match.transport").size() == 1);

  s.Update(102.6);
  CHECK(h.Requested(3, "relay") && !h.Requested(1, "relay") && !h.Requested(1, "direct") && !h.Requested(4, "relay"));
  CHECK(h.Ops("match.transport").size() == 2);
  s.OnTransport(2, 3, "relay", 2, 102.8);
  s.OnTransport(1, 2, "relay", 1, 102.9);
  CHECK(s.peer(3)->transport == MatchTransport::kRelay && s.peer(3)->relay_used);
  CHECK(s.peer(4)->transport == MatchTransport::kDirect && !s.peer(4)->relay_used && s.peer(4)->direct_result == "ok");
  CHECK(s.peer(1)->transport == MatchTransport::kRelay && s.peer(1)->fallback_reason == "chooser_chose_relay");
  CHECK(s.Assignment(3) && s.Assignment(3)->relay && s.Assignment(4) && !s.Assignment(4)->relay);
  CHECK(s.Assignment(1) && s.Assignment(1)->relay && s.Assignments().size() == 3);
  CHECK(h.socket.relay_selected(3) && h.socket.relay_selected(1) && !h.socket.relay_selected(4));

  const auto a1 = s.Assignment(1);
  const auto a4 = s.Assignment(4);
  const PeerView v1 = *s.peer(1);
  const PeerView v4 = *s.peer(4);

  s.OnRelayClosed(3, "rate_limit", 103);
  CHECK(s.peer(3)->transport == MatchTransport::kRelay && s.peer(3)->failure == "relay_closed");
  CHECK(s.peer(3)->relay_result == "failed:relay_closed");
  CHECK(SameAssignment(s.Assignment(1), a1) && SameAssignment(s.Assignment(4), a4));
  CHECK(s.peer(1)->transport == v1.transport && s.peer(4)->transport == v4.transport && s.peer(1)->failure.empty() &&
        s.peer(4)->failure.empty());
  CHECK(!s.fatal());

  s.OnPeerLeft(3, "left", 103.5);
  CHECK(!s.peer(3) && !s.Assignment(3));
  CHECK(SameAssignment(s.Assignment(1), a1) && SameAssignment(s.Assignment(4), a4));
  CHECK(s.peer(1)->transport == MatchTransport::kRelay && s.peer(4)->transport == MatchTransport::kDirect);
  CHECK(s.peer(1)->failure.empty() && s.peer(4)->failure.empty());
  CHECK(!s.fatal() && s.Assignments().size() == 2);
  CHECK(h.socket.relay_selected(1) && !h.socket.relay_selected(3));
  s.Update(104);
  s.OnPeerLost(3);
  CHECK(s.peers().size() == 2 && s.peer(4)->transport == MatchTransport::kDirect);

  s.OnMembers(Members({1, 2, 4}), 105);
  CHECK(s.peers().size() == 2 && !s.fatal());
  s.OnMembers(Members({1, 2}), 106);
  CHECK(s.peers().size() == 1 && s.peer(1) && !s.peer(4) && !s.Assignment(4));
  CHECK(SameAssignment(s.Assignment(1), a1) && !s.fatal());
  CHECK(h.EventCount("peer_disconnect", 3) >= 1 && h.EventCount("peer_disconnect", 4) == 1);
}

void TestPeerLeavingBeforeAnyAssignmentDoesNotMoveTheOthers() {
  Options o = Opts(2);
  o.mesh = true;
  Harness h(o);
  Remote p3, p4;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2, 3, 4}), 100);
  s.Update(100);
  s.OnPeer(Info(3, p3.ep()), 100.1);
  s.OnPeer(Info(4, p4.ep()), 100.1);
  s.OnPeerLeft(3, "left", 100.5);
  CHECK(!s.peer(3) && s.peer(4) && s.peer(4)->transport == MatchTransport::kDirectProbing && s.peer(4)->failure.empty());
  SendPunch(p4, h, PunchKind::kAck, 2, PairPunchKey("match1", 2, 4));
  CHECK(Until(h, 101, [&] { return s.peer(4)->transport == MatchTransport::kDirect; }));
  CHECK(s.Assignment(4) && !s.Assignment(4)->relay && !s.fatal());
}

void TestFatalOnlyForThePairWithTheHost() {
  {
    Harness h(Opts(3));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3}), 100);
    CHECK(s.peers().size() == 1 && s.peer(1) && !s.peer(2));
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    s.OnPeer(Info(2, p1.ep()), 100.1);
    CHECK(s.peers().size() == 1 && !s.fatal());
    s.OnPeerLost(1);
    CHECK(s.peer(1)->member == MemberState::kDisconnected && !s.fatal());
    s.OnMembers(Members({2, 3}), 101);
    CHECK(s.fatal() && !s.peer(1));
    CHECK(h.Event("peer_disconnect", 1) && Harness::Field(h.Event("peer_disconnect", 1), "reason") == "left");
  }
  {
    Options o = Opts(3);
    o.mesh = true;
    Harness h(o);
    Remote p1, p2, p4;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3, 4}), 100);
    s.Update(100);
    s.OnPeer(Info(1, p1.ep()), 100.1);
    s.OnPeer(Info(2, p2.ep()), 100.1);
    s.OnPeer(Info(4, p4.ep()), 100.1);
    CHECK(s.peers().size() == 3);
    s.OnPeerLeft(2, "left", 101);
    CHECK(!s.fatal() && !s.peer(2));
    s.OnRelayClosed(4, "peer_disconnected", 101.5);
    CHECK(s.peer(4)->transport == MatchTransport::kFailed && s.peer(4)->failure == Reason(reason::peer_disconnected));
    CHECK(!s.fatal() && s.peer(1)->transport == MatchTransport::kDirectProbing && s.peer(1)->failure.empty());
    s.OnRelayClosed(1, "peer_disconnected", 102);
    CHECK(s.peer(1)->transport == MatchTransport::kFailed && s.peer(1)->failure == Reason(reason::peer_disconnected));
    CHECK(s.fatal());
  }
  {
    Harness h(Opts(1));
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3}), 100);
    s.Update(100);
    s.OnRelayClosed(2, "peer_disconnected", 101);
    s.OnPeerLeft(3, "left", 101);
    s.OnMembers(Members({1}), 102);
    CHECK(s.peers().empty() && !s.fatal());
  }
  {
    Harness h(Opts(2));
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.Update(114.9);
    CHECK(!s.fatal() && s.peer(1)->transport == MatchTransport::kDirectProbing);
    s.Update(116);
    CHECK(s.fatal() && s.peer(1)->transport == MatchTransport::kFailed);
    CHECK(s.peer(1)->failure == Reason(reason::establishment_timeout));
    CHECK(h.NoticeHas("Could not connect to P1."));
  }
  {
    const auto release = std::make_shared<std::atomic<bool>>(false);
    Options o = Opts(2);
    o.sync = false;
    o.resolve = [release](const std::string&) {
      while (!*release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return std::vector<Endpoint>();
    };
    Harness h(o);
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.Update(116);
    CHECK(s.fatal() && s.peer(1)->failure == Reason(reason::establishment_timeout));
    *release = true;
  }
  {
    Options o = Opts(2);
    o.open = false;
    Harness h(o);
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    CHECK(s.fatal() && s.peer(1)->failure == "socket_unbound" && s.peer(1)->transport == MatchTransport::kFailed);
    s.Update(101);
    CHECK(s.fatal() && h.Ops("match.endpoint").empty());
  }
  {
    Options o = Opts(1);
    o.open = false;
    Harness h(o);
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    CHECK(!s.fatal() && s.peer(2)->failure == "socket_unbound");
    s.Update(101);
  }
}

void TestRelayClosedMarksOnlyThatPeer() {
  {
    Harness h(Opts(1));
    Remote p4;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3, 4}), 100);
    s.Update(100);
    s.OnRelayClosed(2, "peer_disconnected", 100.5);
    CHECK(s.peer(2)->transport == MatchTransport::kFailed && s.peer(2)->failure == Reason(reason::peer_disconnected));
    CHECK(s.peer(2)->direct_result == "failed:" + Reason(reason::peer_disconnected));
    CHECK(s.peer(3)->failure.empty() && s.peer(3)->transport == MatchTransport::kDirectProbing && s.peer(3)->relay_available);
    CHECK(s.peer(4)->failure.empty() && s.peer(4)->transport == MatchTransport::kDirectProbing);

    s.OnRelayClosed(3, "rate_limit", 100.6);
    CHECK(s.peer(3)->failure.empty() && s.peer(3)->transport == MatchTransport::kDirectProbing && !s.peer(3)->relay_available);
    CHECK(s.peer(4)->relay_available && s.peer(4)->failure.empty());
    CHECK(!s.fatal() && h.Ops("match.transport").empty());

    s.OnPeer(Info(4, p4.ep()), 100.7);
    s.Update(102.6);
    CHECK(h.Requested(4, "relay") && !h.Requested(3, "relay") && !h.Requested(2, "relay"));
    s.OnRelayClosed(4, "rate_limit", 102.7);
    const PeerView v4 = *s.peer(4);
    CHECK(v4.transport == MatchTransport::kFailed && v4.failure == Reason(reason::relay_unavailable));
    CHECK(v4.relay_result == "failed:" + Reason(reason::relay_unavailable));
    CHECK(v4.fallback_reason == Reason(reason::direct_timeout));
    CHECK(s.peer(3)->failure.empty() && s.peer(2)->failure == Reason(reason::peer_disconnected));
    s.OnTransport(1, 4, "relay", 1, 103);
    CHECK(!s.Assignment(4) && s.peer(4)->transport == MatchTransport::kFailed);

    s.Update(110.5);
    CHECK(s.peer(3)->transport == MatchTransport::kFailed);
    CHECK(s.peer(3)->failure == "no_direct_path" || s.peer(3)->failure == Reason(reason::establishment_timeout));
  }
  {
    Harness h(Opts(1));
    Remote p2, p4;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3, 4}), 100);
    s.Update(100);
    s.OnPeer(Info(2, p2.ep()), 100.1);
    s.OnPeer(Info(4, p4.ep()), 100.1);
    SendPunch(p2, h, PunchKind::kAck, 2, PairPunchKey("match1", 1, 2));
    CHECK(Until(h, 101, [&] { return s.peer(2)->transport == MatchTransport::kDirect; }));
    s.Update(102.6);
    s.OnTransport(1, 4, "relay", 1, 102.7);
    CHECK(s.peer(4)->transport == MatchTransport::kRelay && s.peer(3)->relay_available);
    const auto a2 = s.Assignment(2);

    s.OnRelayClosed(0, "service_disconnected", 103);
    CHECK(s.peer(2)->transport == MatchTransport::kDirect && s.peer(2)->failure.empty() && SameAssignment(s.Assignment(2), a2));
    CHECK(s.peer(4)->transport == MatchTransport::kRelay && s.peer(4)->failure.empty() && s.Assignment(4).has_value());
    CHECK(s.peer(3)->transport == MatchTransport::kFailed && s.peer(3)->failure == Reason(reason::relay_unavailable));
    CHECK(!s.fatal());
    s.Update(104);
    CHECK(s.peer(2)->transport == MatchTransport::kDirect);
  }
  {
    Harness h(Opts(1));
    Remote p2;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 100);
    s.Update(100);
    s.OnPeer(Info(2, p2.ep()), 100.1);
    s.OnRelayClosed(2, "rate_limit", 100.2);
    CHECK(!s.peer(2)->relay_available && s.peer(2)->failure.empty());
    SendPunch(p2, h, PunchKind::kAck, 2, PairPunchKey("match1", 1, 2));
    CHECK(Until(h, 101, [&] { return s.peer(2)->transport == MatchTransport::kDirect; }));
    CHECK(s.Assignment(2) && !s.Assignment(2)->relay);
    // The other end's relay may be fine, so it still waits for this side's choice.
    CHECK(h.Ops("match.transport").size() == 1);
  }
}

void TestMembershipStates() {
  Harness h(Opts(1));
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}, "joined"), 100);
  CHECK(s.peer(2) && s.peer(2)->transport == MatchTransport::kPending && s.peer(2)->member == MemberState::kJoined);
  s.Update(100);
  CHECK(h.Ops("match.endpoint").size() == 1 && s.peer(2)->transport == MatchTransport::kPending);
  s.OnMembers(Members({1, 2}, "connecting"), 101);
  CHECK(s.peer(2)->transport == MatchTransport::kDirectProbing && s.peer(2)->member == MemberState::kConnecting);
  s.OnMembers(Members({1, 2}, "disconnected"), 102);
  CHECK(s.peer(2)->member == MemberState::kDisconnected && s.peer(2)->transport == MatchTransport::kDirectProbing);
  s.SetPeerLink(2, NetplayLink::kFighting);
  CHECK(s.peer(2)->link == NetplayLink::kFighting);
  s.SetPeerLink(3, NetplayLink::kFighting);
  CHECK(!s.peer(3) && s.match() == "match1" && s.my_slot() == 1);
}

void TestOutOfRangeSlotsAreIgnored() {
  Harness h(Opts(1));
  MatchSession& s = *h.session;
  Remote p;
  s.OnMembers(Members({0, 5, -1, 100, 1, 2, 6}), 100);
  CHECK(s.peers().size() == 1 && s.peer(2));
  for (const int slot : {0, 1, 5, -1, 100, 6}) CHECK(!s.peer(slot) && !s.Assignment(slot));
  s.Update(100);
  for (const int slot : {0, 5, -3, 9}) s.OnPeer(Info(slot, p.ep()), 100.1);
  CHECK(s.peers().size() == 1);
  s.OnTransport(1, 5, "relay", 1, 100.2);
  s.OnTransport(0, 9, "direct", 0, 100.2);
  s.OnTransport(1, 1, "relay", 1, 100.2);
  s.OnTransport(-1, 1, "relay", 1, 100.2);
  s.OnPeerLeft(5, "left", 100.3);
  s.OnPeerLeft(0, "left", 100.3);
  s.OnPeerLeft(-2, "left", 100.3);
  s.OnPeerLost(5);
  s.OnPeerLost(-1);
  s.OnRelayClosed(5, "peer_disconnected", 100.4);
  s.OnRelayClosed(-1, "peer_disconnected", 100.4);
  s.SetPeerLink(5, NetplayLink::kConnected);
  s.SetPeerLink(0, NetplayLink::kConnected);
  s.Update(101);
  CHECK(s.peers().size() == 1 && s.peer(2)->failure.empty() && s.peer(2)->transport == MatchTransport::kDirectProbing);
  CHECK(s.Assignments().empty() && !s.fatal());
  CHECK(h.EventCount("peer_disconnect", 5) == 0 && h.EventCount("peer_disconnect", 0) == 0);

  Harness g(Opts(3));
  g.session->OnMembers(Members({2, 3, 4, 5}), 100);
  CHECK(g.session->peers().empty());
  g.session->OnPeer(Info(2, p.ep()), 100.1);
  g.session->OnPeer(Info(4, p.ep()), 100.1);
  CHECK(g.session->peers().empty());
  g.session->Update(101);
  CHECK(g.session->peers().empty() && g.texts.empty() && !g.session->fatal());
}

void TestWeirdTimesNeverCrash() {
  {
    Options o = Opts(1);
    o.mesh = true;
    Harness h(o);
    Remote p2, p3;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2, 3}), 100);
    s.Update(100);
    s.OnPeer(Info(2, p2.ep()), 100.1);
    s.OnPeer(Info(3, p3.ep()), 100.1);
    for (const double t : {50.0, 100.0, 99.0, -5.0, 0.0, -1e9, 101.0, 1e-9, 100.2}) {
      s.Update(t);
      s.OnTransport(1, 2, "relay", 1, t);
      s.OnRelayClosed(0, "x", t);
      s.OnMembers(Members({1, 2, 3}), t);
    }
    CHECK(s.peers().size() == 2);
    CHECK(s.peer(2)->transport != MatchTransport::kRelay || s.Assignment(2));
    CHECK(!s.fatal());
    s.Update(1e6);
    for (const int slot : {2, 3}) {
      CHECK(s.peer(slot) && (s.peer(slot)->transport == MatchTransport::kFailed || s.peer(slot)->transport == MatchTransport::kRelay));
    }
    s.Update(-1e6);
    s.Update(200);
    s.Update(1e6);
    s.OnPeer(Info(2, p2.ep()), -3);
    s.OnTransport(1, 2, "relay", 1, 5);
    CHECK(s.peers().size() == 2);
  }
  {
    Harness h(Opts(2));
    Remote p1;
    MatchSession& s = *h.session;
    s.OnMembers(Members({1, 2}), 5000);
    s.Update(5000);
    s.OnPeer(Info(1, p1.ep()), 5000.1);
    s.Update(10);
    s.Update(4000);
    CHECK(s.peer(1)->transport == MatchTransport::kDirectProbing && h.Ops("match.transport").empty());
    s.Update(5000.2);
    s.Update(86400.0 * 365);
    CHECK(s.peer(1)->transport == MatchTransport::kFailed && s.fatal());
    s.Update(0);
    s.Update(-100);
  }
}

void TestStopAndGarbageFrames() {
  Harness h(Opts(1));
  Remote p2;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  s.Update(100);
  s.OnPeer(Info(2, p2.ep()), 100.1);
  s.OnBinary("");
  s.OnBinary("garbage");
  s.OnBinary(std::string(40, '\xff'));
  CHECK(s.peer(2)->traffic.relay_rx_packets == 0);
  s.Update(102.6);
  s.OnTransport(1, 2, "relay", 1, 102.7);
  CHECK(s.Assignment(2) && h.socket.relay_open());
  s.Stop();
  CHECK(!h.socket.relay_open());
  s.Update(103);
  s.OnBinary("garbage");
  s.Stop();
  CHECK(s.peers().size() == 1 && s.Assignment(2));
  CHECK(h.LogHas("transport summary peer=2"));
}

void TestRelayFramesCarryTheTokenOnlyInBinary() {
  Harness h(Opts(1));
  Remote p2;
  MatchSession& s = *h.session;
  s.OnMembers(Members({1, 2}), 100);
  s.Update(100);
  s.OnPeer(Info(2, p2.ep()), 100.1);
  s.Update(102.6);
  s.OnTransport(1, 2, "relay", 1, 102.7);
  for (int i = 0; i < 5; ++i) {
    s.Update(103 + i);
    Sleep(5);
  }
  for (const std::vector<uint8_t>& frame : h.frames) {
    const auto decoded = tmnt::netplay::DecodeRelayFrame(frame.data(), frame.size());
    CHECK(decoded && decoded->token == kToken && decoded->src == 1 && decoded->dst == 2);
  }
  for (const std::string& text : h.texts) CHECK(text.find(std::to_string(kToken)) == npos);
}

bool LooksStable(const std::string& code) {
  return !code.empty() && std::all_of(code.begin(), code.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

void TestNothingSensitiveIsLoggedOrReported() {
  CHECK(g_scan.size() > 100);
  char lower[16], upper[16];
  std::snprintf(lower, sizeof lower, "%08x", kToken);
  std::snprintf(upper, sizeof upper, "%08X", kToken);
  const std::string digits = std::to_string(kToken);
  const std::regex hex32("[0-9a-fA-F]{32}");
  const std::regex friend_code("[A-Z0-9]{4}-[A-Z0-9]{4}");
  int token_hits = 0, hex_hits = 0, code_hits = 0;
  for (const std::string& text : g_scan) {
    token_hits += text.find(digits) != npos || text.find(lower) != npos || text.find(upper) != npos;
    hex_hits += std::regex_search(text, hex32);
    code_hits += std::regex_search(text, friend_code);
  }
  CHECK(token_hits == 0);
  CHECK(hex_hits == 0);
  CHECK(code_hits == 0);
  CHECK(!g_reasons.empty());
  for (const std::string& value : g_reasons) CHECK(LooksStable(value) && !ReasonCategory(value).empty());
  CHECK(!g_event_fields.empty());
  for (const auto& [key, value] : g_event_fields) CHECK(SanitizeTelemetryValue(value) == value);
}

}  // namespace

int main() {
  std::printf("online match session tests\n");
  TestStunAndEndpointAdvertisement();
  TestARelayedPairWaitsForTheService();
  TestStunThatNeverAnswersStillAdvertises();
  TestAsynchronousStunLookup();
  TestHostFallsBackToTheRelay();
  TestDirectSuccessSelectsDirect();
  TestServiceThatForwardsNoPublicEndpoints();
  TestEndpointAndStunOrderDoNotChangeTheOutcome();
  TestDirectLateSuccessIsTheSameInEitherOrder();
  TestNonChooserFollowsTheChooser();
  TestPairsAreIndependent();
  TestPeerLeavingBeforeAnyAssignmentDoesNotMoveTheOthers();
  TestFatalOnlyForThePairWithTheHost();
  TestRelayClosedMarksOnlyThatPeer();
  TestMembershipStates();
  TestOutOfRangeSlotsAreIgnored();
  TestWeirdTimesNeverCrash();
  TestStopAndGarbageFrames();
  TestRelayFramesCarryTheTokenOnlyInBinary();
  TestNothingSensitiveIsLoggedOrReported();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
