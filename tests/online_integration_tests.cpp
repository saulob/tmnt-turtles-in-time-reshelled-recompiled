// Four clients against a real online service: node server/tmnt-lobby/local/server.js, then
//   TMNT_LOBBY_TEST_URL=ws://127.0.0.1:8791/ tmnt_online_integration_tests
// Exits 77 (not run) without the variable. Only the online protocol and loopback UDP run; no game, no window.
// One host and three guests: friends, three invites into one hidden match, 4/4, a Direct or Relay path for every pair
// (mixed, when a simulated firewall blocks some pairs), a fifth player turned away, one guest leaving. Then several
// matches in one run, by search and by invite, with the host changing, each starting from nothing.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "netplay/online_game_socket.h"
#include "online/online_service.h"

using namespace tmnt::online;
namespace np = tmnt::netplay;

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

double Now() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// A room connection the test can lose, as a dropped network would.
class LosableChannel final : public LobbyChannel {
 public:
  LosableChannel(std::unique_ptr<LobbyChannel> inner, std::atomic<bool>& lose) : inner_(std::move(inner)), lose_(lose) {}
  bool Poll(ChannelEvent& out) override {
    if (lose_.exchange(false)) {
      inner_->Close();
      out = {ChannelEvent::Kind::kClosed, "lost"};
      return true;
    }
    return inner_->Poll(out);
  }
  void Send(const std::string& text) override { inner_->Send(text); }
  bool SendBinary(std::vector<uint8_t>&& bytes) override { return inner_->SendBinary(std::move(bytes)); }
  void Close() override { inner_->Close(); }

 private:
  std::unique_ptr<LobbyChannel> inner_;
  std::atomic<bool>& lose_;
};

struct Client {
  np::OnlineGameSocket socket;  // outlives the service, which taps it
  OnlineService service;
  std::string player;
  std::string name;
  std::mutex mutex;
  std::vector<std::string> events;
  std::vector<std::string> logs;
  std::atomic<bool> lose_room{false};

  Client(const std::string& url, char fill, const char* display_name, uint16_t port)
      : service(Config(url, fill)), player(DerivePlayerId(std::string(64, fill))), name(display_name) {
    socket.Open({port}, true);
    service.Apply({true, display_name});
  }

  ServiceConfig Config(const std::string& url, char fill) {
    ServiceConfig config;
    config.url = url;
    config.version = "0.2.2";
    config.platform = "Windows";
    config.lan = {"127.0.0.1"};
    config.stun = "off";
    config.synchronous_probes = true;
    config.game_socket = &socket;
    config.identity = [fill] {
      Identity id;
      id.token = std::string(64, fill);
      id.player_id = DerivePlayerId(id.token);
      return id;
    };
    config.event = [this](const SessionEvent& event) {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back(event.name);
    };
    config.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      logs.push_back(line);
    };
    config.open_channel = [this](const std::string& channel_url) -> std::unique_ptr<LobbyChannel> {
      std::unique_ptr<LobbyChannel> link = OpenLobbyLink(channel_url);
      if (channel_url.find("/match/") == std::string::npos) return link;
      return std::make_unique<LosableChannel>(std::move(link), lose_room);
    };
    return config;
  }

  bool Logged(const std::string& part) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& line : logs) {
      if (line.find(part) != std::string::npos) return true;
    }
    return false;
  }

  bool Saw(const std::string& event) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& seen : events) {
      if (seen == event) return true;
    }
    return false;
  }

  std::optional<PeerView> PeerOf(int slot) {
    for (const PeerView& peer : service.snapshot().peers) {
      if (peer.slot == slot) return peer;
    }
    return std::nullopt;
  }

  bool PeerReady(int slot) {
    const auto peer = PeerOf(slot);
    return peer && (peer->transport == MatchTransport::kDirect || peer->transport == MatchTransport::kRelay);
  }
};

using Clients = std::vector<Client*>;

bool WaitFor(const std::function<bool()>& done, const Clients& clients, double seconds = 25.0) {
  const double end = Now() + seconds;
  while (Now() < end) {
    for (Client* client : clients) client->service.Update(Now());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool HasFriend(Client& client, const std::string& code) {
  for (const Friend& f : client.service.snapshot().friends) {
    if (f.player == code) return true;
  }
  return false;
}

bool BecomeFriends(Client& a, Client& b, const Clients& clients) {
  const auto online = [&] {
    return a.service.snapshot().connection == Connection::kOnline && b.service.snapshot().connection == Connection::kOnline &&
           !a.service.snapshot().friend_code.empty() && !b.service.snapshot().friend_code.empty();
  };
  if (!WaitFor(online, clients)) return false;
  if (!WaitFor([&] { return a.service.snapshot().fetched && b.service.snapshot().fetched; }, clients)) return false;
  if (HasFriend(a, b.service.snapshot().friend_code) && HasFriend(b, a.service.snapshot().friend_code)) return true;
  if (a.service.AddFriend(b.service.snapshot().friend_code) != FriendRequestResult::kSent) return false;
  if (!WaitFor([&] { return !b.service.snapshot().incoming.empty(); }, clients)) return false;
  b.service.AcceptFriend(a.service.snapshot().friend_code);
  return WaitFor([&] {
    return HasFriend(a, b.service.snapshot().friend_code) && HasFriend(b, a.service.snapshot().friend_code);
  }, clients);
}

// The host (slot 1) invites three friends; every invite leads into the same hidden match.
bool FormParty(Client& host, Client& b, Client& c, Client& d, const Clients& clients) {
  if (!host.service.CreateParty()) return false;
  if (!WaitFor([&] { return host.service.party().has_value(); }, clients)) return false;
  for (Client* guest : {&b, &c, &d}) {
    if (!host.service.InviteToParty(guest->service.snapshot().friend_code)) return false;
  }
  for (Client* guest : {&b, &c, &d}) {
    if (!WaitFor([&] { return !guest->service.Invites().empty(); }, clients)) return false;
    if (!guest->service.AcceptPartyInvite(guest->service.Invites().front().id)) return false;
    if (!WaitFor([&] { return guest->service.party().has_value(); }, clients)) return false;
  }
  return WaitFor([&] { return host.service.party() && host.service.party()->members.size() == 4; }, clients);
}

bool AllPairsConnected(const Clients& clients, const std::set<int>& slots) {
  for (Client* client : clients) {
    const int mine = client->service.my_slot();
    for (const int other : slots) {
      if (other != mine && !client->PeerReady(other)) return false;
    }
  }
  return true;
}

// A datagram from `from` reaches `to` over whichever path the pair chose.
bool Delivers(Client& from, Client& to, const Clients& clients) {
  const int to_slot = to.service.my_slot();
  const int from_slot = from.service.my_slot();
  const auto assignment = from.service.Assignment(to_slot);
  if (!assignment) return false;
  uint32_t address = 0;
  if (!np::UsableAddress(np::ParsePeerAddress(assignment->peer_ip, address))) return false;
  const std::string text = "HELLO " + std::to_string(from_slot) + ">" + std::to_string(to_slot);
  if (!from.socket.Send({address, assignment->peer_port}, reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
    return false;
  }
  return WaitFor([&] {
    uint8_t buffer[64];
    np::Endpoint origin;
    while (const size_t size = to.socket.Receive(buffer, sizeof buffer, origin)) {
      if (std::string(reinterpret_cast<char*>(buffer), size) == text) return true;
    }
    return false;
  }, clients, 5.0);
}

void Leave(const Clients& clients) {
  for (Client* client : clients) client->service.Apply({false, client->name});
}

void TestFourPlayersAllDirect(const std::string& url) {
  std::printf("  four players, all direct\n");
  Client a(url, 'a', "Alpha", 58201), b(url, 'b', "Bravo", 58202), c(url, 'c', "Charlie", 58203), d(url, 'd', "Delta", 58204);
  const Clients clients = {&a, &b, &c, &d};
  CHECK(BecomeFriends(a, b, clients) && BecomeFriends(a, c, clients) && BecomeFriends(a, d, clients));
  CHECK(FormParty(a, b, c, d, clients));

  const Party party = *a.service.party();
  CHECK(party.members.size() == 4 && party.host == a.service.snapshot().friend_code);
  for (int slot = 1; slot <= 4; ++slot) CHECK(party.member(slot));
  CHECK(a.service.my_slot() == 1 && b.service.my_slot() == 2 && c.service.my_slot() == 3 && d.service.my_slot() == 4);
  CHECK(b.service.party()->match == party.match && c.service.party()->match == party.match &&
        d.service.party()->match == party.match);  // one hidden match for all
  CHECK(!party.diag.empty() && b.service.party()->diag == party.diag);

  CHECK(WaitFor([&] { return AllPairsConnected(clients, {1, 2, 3, 4}); }, clients));
  for (Client* client : clients) {
    const std::vector<PeerView> peers = client->service.snapshot().peers;
    CHECK(peers.size() == 3);  // the game's own topology is a full mesh
    for (const PeerView& peer : peers) CHECK(peer.transport == MatchTransport::kDirect && !peer.relay_used);
    CHECK(client->service.Assignments().size() == 3);
  }
  for (Client* from : clients) {
    for (Client* to : clients) {
      if (from != to) CHECK(Delivers(*from, *to, clients));
    }
  }
  CHECK(WaitFor([&] {
    for (Client* client : clients) {
      const std::vector<PeerView> peers = client->service.snapshot().peers;
      if (peers.size() != 3) return false;
      for (const PeerView& peer : peers) {
        if (!peer.rtt_ms) return false;
      }
    }
    return true;
  }, clients, 15.0));
  for (Client* client : clients) {
    for (const PeerView& peer : client->service.snapshot().peers) CHECK(peer.rtt_ms && *peer.rtt_ms < 1000);
  }

  // The game's own session advertisement reaches only the party, four slots wide.
  a.service.HostPlayerMatch({}, 4);
  CHECK(WaitFor([&] { return !a.service.snapshot().player_match.session.empty(); }, clients));
  b.service.SetPlayerMatchSearch(true);
  CHECK(WaitFor([&] { return !b.service.PlayerMatchResults().empty(); }, clients));
  CHECK(b.service.PlayerMatchResults().front().slots == 4);
  CHECK(b.service.JoinPlayerMatch(b.service.PlayerMatchResults().front().session));

  // A fifth player cannot enter a full party.
  Client e(url, 'e', "Echo", 58205);
  const Clients with_e = {&a, &b, &c, &d, &e};
  CHECK(BecomeFriends(a, e, with_e));
  a.service.InviteToParty(e.service.snapshot().friend_code);
  CHECK(!WaitFor([&] { return e.service.party().has_value(); }, with_e, 2.0));
  CHECK(a.service.party()->members.size() == 4);

  // The game starts: every member then exchanges game traffic with every other member.
  for (Client* client : clients) client->service.NoteGameStart();
  for (Client* client : clients) {
    for (const PeerView& peer : client->service.snapshot().peers) client->service.SetPeerLink(peer.slot, NetplayLink::kConnected);
  }
  CHECK(WaitFor([&] {
    for (Client* client : clients) {
      if (!client->Saw("game_start_success")) return false;
    }
    return true;
  }, clients, 10.0));
  for (Client* client : clients) CHECK(client->Saw("game_start_attempt") && !client->Saw("game_start_failure"));

  // One guest leaving takes only its own links away.
  d.service.LeaveParty();
  CHECK(WaitFor([&] { return a.service.party() && a.service.party()->members.size() == 3; }, with_e));
  CHECK(WaitFor([&] { return a.service.snapshot().peers.size() == 2 && b.service.snapshot().peers.size() == 2; }, with_e));
  CHECK(a.PeerReady(2) && a.PeerReady(3) && b.PeerReady(1) && b.PeerReady(3) && c.PeerReady(1) && c.PeerReady(2));
  CHECK(Delivers(a, b, with_e) && Delivers(b, c, with_e));

  // The events a support report needs.
  CHECK(b.Saw("invite_received") && b.Saw("join_completed") && b.Saw("transport_selected"));
  CHECK(a.Saw("direct_attempt") && a.Saw("direct_result") && a.Saw("transport_selected"));
  Leave(with_e);
}

void TestARelayedPairOutlivesALostRoom(const std::string& url) {
  std::printf("  a relayed pair outlives a lost room connection\n");
  Client a(url, '5', "Alpha", 58221), b(url, '6', "Bravo", 58222);
  const Clients clients = {&a, &b};
  const uint16_t a_port = a.socket.bound_port();
  const uint16_t b_port = b.socket.bound_port();
  a.socket.SetDirectFilter([b_port](const np::Endpoint& endpoint) { return endpoint.port != b_port; });
  b.socket.SetDirectFilter([a_port](const np::Endpoint& endpoint) { return endpoint.port != a_port; });
  CHECK(BecomeFriends(a, b, clients));
  CHECK(a.service.CreateParty());
  CHECK(WaitFor([&] { return a.service.party().has_value(); }, clients));
  CHECK(a.service.InviteToParty(b.service.snapshot().friend_code));
  CHECK(WaitFor([&] { return !b.service.Invites().empty(); }, clients));
  CHECK(b.service.AcceptPartyInvite(b.service.Invites().front().id));
  CHECK(WaitFor([&] { return AllPairsConnected(clients, {1, 2}); }, clients, 40.0));
  CHECK(a.PeerOf(2)->transport == MatchTransport::kRelay && b.PeerOf(1)->transport == MatchTransport::kRelay);
  CHECK(Delivers(a, b, clients) && Delivers(b, a, clients));

  // The guest's connection goes, then the host's: each side waits, and the pair carries on over the new one.
  b.lose_room = true;
  CHECK(WaitFor([&] { return b.Logged("relay resumed peer=1") && a.Logged("relay resumed peer=2"); }, clients));
  CHECK(a.Logged("relay suspended peer=2") && b.Logged("relay suspended peer=1"));
  CHECK(Delivers(a, b, clients) && Delivers(b, a, clients));
  a.lose_room = true;
  CHECK(WaitFor([&] { return a.service.snapshot().room_state != "online"; }, clients, 2.0));
  CHECK(WaitFor([&] { return a.service.snapshot().room_state == "online"; }, clients));
  CHECK(WaitFor([&] { return Delivers(a, b, clients) && Delivers(b, a, clients); }, clients, 10.0));
  CHECK(a.PeerOf(2)->failure.empty() && b.PeerOf(1)->failure.empty());
  CHECK(a.service.party() && b.service.party() && a.service.Assignment(2) && b.service.Assignment(1));
  Leave(clients);
}

void TestMixedDirectAndRelay(const std::string& url) {
  std::printf("  mixed direct and relay\n");
  Client a(url, '1', "Alpha", 58211), b(url, '2', "Bravo", 58212), c(url, '3', "Charlie", 58213), d(url, '4', "Delta", 58214);
  const Clients clients = {&a, &b, &c, &d};
  // A firewall between Bravo and Charlie, and between Alpha and Delta: those pairs must use the relay.
  const auto block_between = [](Client& x, Client& y) {
    const uint16_t x_port = x.socket.bound_port();
    const uint16_t y_port = y.socket.bound_port();
    x.socket.SetDirectFilter([y_port](const np::Endpoint& endpoint) { return endpoint.port != y_port; });
    y.socket.SetDirectFilter([x_port](const np::Endpoint& endpoint) { return endpoint.port != x_port; });
  };
  block_between(b, c);
  block_between(a, d);

  CHECK(BecomeFriends(a, b, clients) && BecomeFriends(a, c, clients) && BecomeFriends(a, d, clients));
  CHECK(FormParty(a, b, c, d, clients));
  CHECK(WaitFor([&] { return AllPairsConnected(clients, {1, 2, 3, 4}); }, clients, 40.0));

  const auto transport = [](Client& client, int slot) {
    const auto peer = client.PeerOf(slot);
    return peer ? peer->transport : MatchTransport::kPending;
  };
  // Every pair, from both ends, agrees on its transport; only the blocked pairs are relayed.
  CHECK(transport(a, 2) == MatchTransport::kDirect && transport(b, 1) == MatchTransport::kDirect);
  CHECK(transport(a, 3) == MatchTransport::kDirect && transport(c, 1) == MatchTransport::kDirect);
  CHECK(transport(a, 4) == MatchTransport::kRelay && transport(d, 1) == MatchTransport::kRelay);
  CHECK(transport(b, 3) == MatchTransport::kRelay && transport(c, 2) == MatchTransport::kRelay);
  CHECK(transport(b, 4) == MatchTransport::kDirect && transport(d, 2) == MatchTransport::kDirect);
  CHECK(transport(c, 4) == MatchTransport::kDirect && transport(d, 3) == MatchTransport::kDirect);

  const auto relayed = a.PeerOf(4);
  CHECK(relayed && relayed->relay_used && !relayed->fallback_reason.empty() && relayed->relay_result == "ok");
  CHECK(!a.PeerOf(2)->relay_used && a.PeerOf(2)->fallback_reason.empty());
  CHECK(a.Saw("relay_fallback") && b.Saw("relay_fallback"));

  for (Client* from : clients) {
    for (Client* to : clients) {
      if (from != to) CHECK(Delivers(*from, *to, clients));
    }
  }
  CHECK(a.socket.peer_traffic(4).relay_tx_packets > 0 && a.socket.peer_traffic(4).udp_tx_packets == 0);
  CHECK(a.socket.peer_traffic(2).relay_tx_packets == 0 && a.socket.peer_traffic(2).udp_tx_packets > 0);

  CHECK(WaitFor([&] {
    for (Client* client : clients) {
      for (const PeerView& peer : client->service.snapshot().peers) {
        if (!peer.rtt_ms) return false;
      }
    }
    return true;
  }, clients, 15.0));

  // The relayed participant leaves: the other pairs, direct and relayed, are not disturbed.
  d.service.LeaveParty();
  CHECK(WaitFor([&] { return a.service.snapshot().peers.size() == 2; }, clients));
  CHECK(a.PeerReady(2) && a.PeerReady(3) && transport(b, 3) == MatchTransport::kRelay && Delivers(b, c, clients));
  Leave(clients);
}

void TestInviteRefusals(const std::string& url) {
  std::printf("  invite refusals\n");
  Client a(url, '5', "Alpha", 58221), b(url, '6', "Bravo", 58222);
  const Clients clients = {&a, &b};
  CHECK(BecomeFriends(a, b, clients));
  CHECK(!a.service.InviteToParty(b.service.snapshot().friend_code));  // no party yet
  CHECK(a.service.CreateParty());
  CHECK(WaitFor([&] { return a.service.party().has_value(); }, clients));
  CHECK(a.service.InviteToParty(b.service.snapshot().friend_code));
  CHECK(WaitFor([&] { return !b.service.Invites().empty(); }, clients));
  const std::string id = b.service.Invites().front().id;
  b.service.DeclinePartyInvite(id);
  CHECK(b.service.Invites().empty() && !b.service.AcceptPartyInvite(id));
  CHECK(!b.service.party().has_value());
  a.service.LeaveParty();
  CHECK(!a.service.party().has_value());
  Leave(clients);
}

void HostMatch(Client& host, int slots, const Clients& clients) {
  host.service.HostPlayerMatch({}, slots);
  CHECK(WaitFor([&] { return !host.service.snapshot().player_match.session.empty() && host.service.party(); }, clients));
}

// The way the game's Quick Match goes: search until the friend's session shows, then join it.
void JoinBySearch(Client& guest, const Clients& clients) {
  CHECK(WaitFor([&] {
    guest.service.AskPlayerMatchSearch();
    return !guest.service.PlayerMatchResults().empty();
  }, clients));
  CHECK(guest.service.JoinPlayerMatch(guest.service.PlayerMatchResults().front().session));
  CHECK(WaitFor([&] { return guest.service.party().has_value(); }, clients));
}

// The way an accepted invite goes: the party first, then the game joins the session the invite names.
void JoinByInvite(Client& host, Client& guest, const Clients& clients) {
  CHECK(host.service.InviteToParty(guest.service.snapshot().friend_code));
  CHECK(WaitFor([&] { return !guest.service.Invites().empty(); }, clients));
  CHECK(guest.service.AcceptPartyInvite(guest.service.Invites().front().id));
  CHECK(WaitFor([&] { return guest.service.AcceptedInviteInfo().has_value(); }, clients));
  const AcceptedInvite accepted = *guest.service.AcceptedInviteInfo();
  CHECK(accepted.slots == host.service.party()->slots);
  CHECK(guest.service.JoinPlayerMatch(accepted.session));
  CHECK(WaitFor([&] { return !guest.service.AcceptedInviteInfo().has_value(); }, clients));
}

void EndMatch(const Clients& players, const Clients& clients) {
  for (Client* player : players) {
    player->service.CancelPlayerMatch();
    player->service.LeaveFinishedMatch();
  }
  CHECK(WaitFor([&] {
    for (Client* player : players) {
      if (player->service.party() || !player->service.Peers().empty() || !player->service.Assignments().empty()) return false;
      if (player->service.snapshot().player_match.hosting) return false;
      for (const Friend& f : player->service.snapshot().friends) {
        if (f.state != FriendState::kOnline) return false;
      }
    }
    return true;
  }, clients));
}

// A datagram sent before a match ended never reaches the next one: its socket starts empty.
void CheckNothingCarriedOver(const Clients& players) {
  for (Client* player : players) {
    uint8_t buffer[64];
    np::Endpoint origin;
    while (const size_t size = player->socket.Receive(buffer, sizeof buffer, origin)) {
      CHECK(std::memcmp(buffer, "HELLO", std::min<size_t>(size, 5)) != 0);
    }
  }
}

void TestMatchesInARow(const std::string& url) {
  std::printf("  matches in a row\n");
  Client a(url, '7', "Alpha", 58231), b(url, '8', "Bravo", 58232), c(url, '9', "Charlie", 58233);
  const Clients clients = {&a, &b, &c};
  CHECK(BecomeFriends(a, b, clients) && BecomeFriends(a, c, clients) && BecomeFriends(b, c, clients));
  std::set<std::string> matches, sessions;
  const auto play = [&](Client& host, const Clients& guests, const Clients& players) {
    const std::string match = host.service.party()->match;
    CHECK(matches.insert(match).second);
    CHECK(sessions.insert(host.service.snapshot().player_match.session).second);
    std::set<int> slots;
    for (Client* player : players) {
      CHECK(player->service.party() && player->service.party()->match == match);
      slots.insert(player->service.my_slot());
    }
    CHECK(slots.size() == players.size() && host.service.my_slot() == 1);
    CHECK(WaitFor([&] { return AllPairsConnected(players, slots); }, clients));
    for (Client* guest : guests) CHECK(Delivers(host, *guest, clients) && Delivers(*guest, host, clients));
  };

  // Create, Quick Match, leave.
  HostMatch(a, 4, clients);
  JoinBySearch(b, clients);
  play(a, {&b}, {&a, &b});
  EndMatch({&a, &b}, clients);
  CheckNothingCarriedOver({&a, &b});

  // Create again, joined by invite.
  HostMatch(a, 2, clients);
  JoinByInvite(a, b, clients);
  play(a, {&b}, {&a, &b});
  CHECK(!a.service.InviteToParty(c.service.snapshot().friend_code) || WaitFor([&] {
    return a.service.snapshot().notice == "The party is full.";
  }, clients));  // two slots, both taken
  EndMatch({&a, &b}, clients);
  CheckNothingCarriedOver({&a, &b});

  // The first guest hosts and the first host joins, by search.
  HostMatch(b, 3, clients);
  JoinBySearch(a, clients);
  play(b, {&a}, {&a, &b});

  // A third player joins the same match by invite, with a guest already in the lobby.
  JoinByInvite(b, c, clients);
  CHECK(WaitFor([&] { return AllPairsConnected({&a, &b, &c}, {1, 2, 3}); }, clients));
  CHECK(Delivers(a, c, clients) && Delivers(c, a, clients));
  EndMatch({&a, &b, &c}, clients);
  CheckNothingCarriedOver({&a, &b, &c});

  // And once more with everyone back where they started.
  HostMatch(a, 3, clients);
  JoinBySearch(c, clients);
  JoinByInvite(a, b, clients);
  CHECK(WaitFor([&] { return AllPairsConnected({&a, &b, &c}, {1, 2, 3}); }, clients));
  CHECK(Delivers(a, b, clients) && Delivers(b, c, clients));
  EndMatch({&a, &b, &c}, clients);
  Leave(clients);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  const char* url = std::getenv("TMNT_LOBBY_TEST_URL");
  if (!url || !*url) {
    std::printf("skipped: TMNT_LOBBY_TEST_URL is not set\n");
    return 77;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("online integration tests (four clients)\n");
  if (only.empty() || only == "direct") TestFourPlayersAllDirect(url);
  if (only.empty() || only == "mixed") TestMixedDirectAndRelay(url);
  if (only.empty() || only == "resume") TestARelayedPairOutlivesALostRoom(url);
  if (only.empty() || only == "invites") TestInviteRefusals(url);
  if (only.empty() || only == "repeat") TestMatchesInARow(url);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
