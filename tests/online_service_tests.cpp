// Online service state machine over a scripted channel: Online off by default, sign-in, friends, the party and its
// invites, the match room, the game-session advertisement, reconnects, telemetry and the connection test.
// Headless: no sockets, no threads, no window.
// Run: tmnt_online_service_tests

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "online/friend_view.h"
#include "online/online_service.h"
#include "online/telemetry.h"

using namespace tmnt::online;


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

constexpr char kToken[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kFriendA[] = "ABCD-2345";
constexpr char kFriendB[] = "EFGH-6789";
constexpr char kSession[] = "00112233445566aa";

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct ChannelState {
  std::string url;
  std::deque<ChannelEvent> in;
  std::vector<std::string> sent;
  std::vector<std::vector<uint8_t>> binary;
  bool closed = false;
};

class FakeChannel final : public LobbyChannel {
 public:
  explicit FakeChannel(std::shared_ptr<ChannelState> state) : state_(std::move(state)) {}
  bool Poll(ChannelEvent& out) override {
    if (state_->in.empty()) return false;
    out = std::move(state_->in.front());
    state_->in.pop_front();
    return true;
  }
  void Send(const std::string& text) override { state_->sent.push_back(text); }
  bool SendBinary(std::vector<uint8_t>&& bytes) override {
    if (state_->closed) return false;
    state_->binary.push_back(std::move(bytes));
    return true;
  }
  void Close() override { state_->closed = true; }

 private:
  std::shared_ptr<ChannelState> state_;
};

int SeqOf(const std::string& request) {
  const size_t at = request.find("\"seq\":");
  return at == std::string::npos ? 0 : std::atoi(request.c_str() + at + 6);
}

std::string FriendJson(const char* player, const char* name, bool online, const char* state = "online",
                       const char* guest = "base", bool compatible = true) {
  return std::string("{\"player\":\"") + player + "\",\"name\":\"" + name + "\",\"online\":" + (online ? "true" : "false") +
         ",\"state\":\"" + (online ? state : "offline") + "\",\"platform\":\"Windows\",\"version\":\"0.2.2\",\"guest\":\"" +
         guest + "\",\"compatible\":" + (compatible ? "true" : "false") + "}";
}

struct Rig {
  std::vector<std::shared_ptr<ChannelState>> channels;
  std::vector<std::string> logs;
  int test_calls = 0;
  bool test_throws = false;
  bool beacon_throws = false;
  std::vector<std::string> beacons;
  std::vector<AcceptedInvite> delivered;
  ConnectionReport test_result;
  ConnectionTestRequest test_request;
  std::unique_ptr<OnlineService> service;
  std::string me;
  double now = 100;

  explicit Rig(const std::string& guest = kGuestBase, bool ephemeral = false, const std::string& token = kToken) {
    ServiceConfig config;
    config.url = "wss://example.test/";
    config.guest = guest;
    config.version = "0.2.2";
    config.platform = "Windows";
    config.synchronous_probes = true;
    config.lan = {"127.0.0.1", "192.168.1.5"};
    config.identity = [ephemeral, token] {
      Identity id;
      id.token = token;
      id.player_id = DerivePlayerId(token);
      id.ephemeral = ephemeral;
      return id;
    };
    config.log = [this](const std::string& line) { logs.push_back(line); };
    config.open_channel = [this](const std::string& url) {
      auto state = std::make_shared<ChannelState>();
      state->url = url;
      channels.push_back(state);
      return std::unique_ptr<LobbyChannel>(new FakeChannel(state));
    };
    config.test = [this](const ConnectionTestRequest& request) {
      ++test_calls;
      test_request = request;
      if (test_throws) throw std::runtime_error("test broke");
      return test_result;
    };
    config.invite_accepted = [this](const AcceptedInvite& invite) { delivered.push_back(invite); };
    config.test_beacon = [this](const std::string& path) {
      beacons.push_back(path);
      if (beacon_throws) throw std::runtime_error("beacon broke");
    };
    service = std::make_unique<OnlineService>(config);
    me = DerivePlayerId(token);
  }

  int CountLogs(const std::string& part) const {
    int n = 0;
    for (const std::string& line : logs) n += Contains(line, part) ? 1 : 0;
    return n;
  }

  ChannelState& Last() {
    for (auto it = channels.rbegin(); it != channels.rend(); ++it) {
      if (!Contains((*it)->url, "/match/")) return **it;
    }
    return *channels.back();
  }
  ChannelState& Room() {
    for (auto it = channels.rbegin(); it != channels.rend(); ++it) {
      if (Contains((*it)->url, "/match/")) return **it;
    }
    return *channels.back();
  }
  void FeedRoom(const std::string& text) {
    Room().in.push_back({ChannelEvent::Kind::kText, text});
    Tick(0.01);
  }
  void EventRoom(ChannelEvent::Kind kind, const std::string& why = {}) {
    Room().in.push_back({kind, why});
    Tick(0.01);
  }
  std::string LastRoomRequest(const std::string& op) {
    for (auto it = Room().sent.rbegin(); it != Room().sent.rend(); ++it) {
      if (Contains(*it, "\"op\":\"" + op + "\"")) return *it;
    }
    return {};
  }
  void Tick(double seconds = 0.1) {
    now += seconds;
    service->Update(now);
  }
  void Feed(const std::string& text) {
    Last().in.push_back({ChannelEvent::Kind::kText, text});
    Tick(0.01);
  }
  void Event(ChannelEvent::Kind kind, const std::string& why = {}) {
    Last().in.push_back({kind, why});
    Tick(0.01);
  }
  int Count(const std::string& op) {
    int n = 0;
    for (const std::string& s : Last().sent) n += Contains(s, "\"op\":\"" + op + "\"") ? 1 : 0;
    return n;
  }
  std::string LastRequest(const std::string& op) {
    for (auto it = Last().sent.rbegin(); it != Last().sent.rend(); ++it) {
      if (Contains(*it, "\"op\":\"" + op + "\"")) return *it;
    }
    return {};
  }
  void Reply(const std::string& op, const std::string& body = {}, const char* code = "ok") {
    const std::string request = LastRequest(op);
    CHECK(!request.empty());
    Feed("{\"re\":" + std::to_string(SeqOf(request)) + ",\"code\":\"" + code + "\"" + (body.empty() ? "" : "," + body) + "}");
  }

  void Enable(const char* name = "Saulo") { service->Apply({true, name}); }

  void SignIn(const char* name = "Saulo") {
    Enable(name);
    Tick();
    Event(ChannelEvent::Kind::kOpen);
    Reply("hello", "\"player\":\"" + me + "\",\"name\":\"" + name + "\",\"friend_code\":\"" + me + "\"");
  }

  void SignInWithFriends() {
    SignIn();
    Reply("friends.list", "\"friends\":[" + FriendJson(kFriendA, "SauloB", true) + "," + FriendJson(kFriendB, "Zed", false) + "]");
    Reply("friends.requests", "\"incoming\":[],\"outgoing\":[]");
  }
};

std::string SearchAnswerFor(int slots) {
  return std::string("\"sessions\":[{\"session\":\"") + kSession + "\",\"player\":\"" + kFriendA +
         "\",\"name\":\"SauloB\",\"guest\":\"base\",\"build\":1,\"slots\":" + std::to_string(slots) +
         ",\"rules\":{},\"platform\":\"Windows\",\"version\":\"0.2.2\"}]";
}


void TestOnlineOffHasNoNetworkBehaviour() {
  Rig rig;
  for (int i = 0; i < 50; ++i) rig.Tick(1.0);
  CHECK(rig.channels.empty());
  rig.service->AddFriend(kFriendA);
  rig.service->HostPlayerMatch({});
  rig.service->CreateParty();
  rig.service->SetPlayerMatchSearch(true);
  CHECK(!rig.service->JoinPlayerMatch(kSession));
  rig.service->TestConnection();
  rig.service->SetPeerLink(2, NetplayLink::kNone);
  rig.Tick();
  CHECK(rig.channels.empty());
  CHECK(rig.test_calls == 0);
  CHECK(!rig.service->Assignment(2) && rig.service->Assignments().empty());
  CHECK(rig.service->snapshot().connection == Connection::kOff);

  rig.Enable();
  rig.Tick();
  CHECK(rig.channels.size() == 1);
  rig.service->Apply({false, "Saulo"});
  const size_t opened = rig.channels.size();
  for (int i = 0; i < 20; ++i) rig.Tick(5.0);
  CHECK(rig.channels.size() == opened);
  CHECK(rig.service->snapshot().connection == Connection::kOff);
}

void TestSignInSendsPresenceDetailsAndNeverTheTokenInLogs() {
  Rig rig;
  rig.SignIn();
  const std::string hello = rig.LastRequest("hello");
  CHECK(Contains(hello, "\"presence\":true") && Contains(hello, "\"platform\":\"Windows\"") &&
        Contains(hello, "\"version\":\"0.2.2\"") && Contains(hello, "\"guest\":\"base\""));
  CHECK(Contains(hello, kToken));
  for (const std::string& line : rig.logs) CHECK(!Contains(line, kToken));
  const Snapshot s = rig.service->snapshot();
  CHECK(s.connection == Connection::kOnline);
  CHECK(s.friend_code == rig.me);
  CHECK(rig.Count("friends.list") == 1 && rig.Count("friends.requests") == 1);
  CHECK(rig.channels[0]->url == "wss://example.test/");
}

void TestFriendsAndPresenceAreShown() {
  Rig rig;
  rig.SignInWithFriends();
  const Snapshot s = rig.service->snapshot();
  CHECK(s.friends.size() == 2 && s.fetched);
  CHECK(FriendStatusLine(s.friends[0]) == "Online \xC2\xB7 Windows \xC2\xB7 v0.2.2");
  CHECK(FriendStatusLine(s.friends[1]) == "Offline");
  CHECK(DescribeFriend(s, s.friends[0]).status == "Online \xC2\xB7 Windows \xC2\xB7 v0.2.2");
  CHECK(!DescribeFriend(s, s.friends[0]).incompatible);

  rig.Feed("{\"ev\":\"friend.status\",\"friend\":" + FriendJson(kFriendB, "Zed", true, "in_game") + "}");
  const Snapshot after = rig.service->snapshot();
  CHECK(after.friends[1].online && FriendStatusLine(after.friends[1]) == "In game \xC2\xB7 Windows \xC2\xB7 v0.2.2");
  rig.Feed("{\"ev\":\"friend.status\",\"friend\":" + FriendJson(kFriendA, "SauloB", true, "player_match") + "}");
  const Snapshot hosting = rig.service->snapshot();
  CHECK(FriendStatusLine(hosting.friends[0]) == "Online Match \xC2\xB7 Windows \xC2\xB7 v0.2.2");
  rig.Feed(std::string("{\"ev\":\"friend.offline\",\"player\":\"") + kFriendB + "\"}");
  CHECK(!rig.service->snapshot().friends[1].online);
}

void TestAddFriendValidatesBeforeSending() {
  Rig rig;
  rig.SignInWithFriends();
  CHECK(rig.service->AddFriend("nonsense") == FriendRequestResult::kInvalid);
  CHECK(Contains(rig.service->snapshot().notice, "not a valid friend code"));
  CHECK(rig.service->AddFriend("ABCD") == FriendRequestResult::kInvalid);
  CHECK(rig.service->AddFriend("ABCD-234") == FriendRequestResult::kInvalid);
  CHECK(rig.service->AddFriend("ABCD-1234") == FriendRequestResult::kInvalid);
  CHECK(rig.service->AddFriend(rig.me) == FriendRequestResult::kOwnCode);
  CHECK(rig.service->snapshot().notice == "That is your own friend code.");
  CHECK(rig.service->AddFriend(kFriendA) == FriendRequestResult::kAlreadyFriends);
  CHECK(rig.service->snapshot().notice == "Already friends.");
  CHECK(rig.Count("friends.add") == 0);
  CHECK(!rig.service->TakeNotices().empty());

  CHECK(rig.service->AddFriend("jklm 3456") == FriendRequestResult::kSent);
  CHECK(rig.Count("friends.add") == 1);
  CHECK(Contains(rig.LastRequest("friends.add"), "\"code\":\"JKLM-3456\""));
  rig.Reply("friends.add", "\"player\":\"JKLM-3456\",\"name\":\"Kim\",\"state\":\"pending\"");
  CHECK(rig.service->snapshot().notice == "Friend request sent.");

  CHECK(rig.service->AddFriend("NPQR-7892") == FriendRequestResult::kSent);
  rig.Reply("friends.add", "", "not_found");
  CHECK(Contains(rig.service->snapshot().notice, "No player has that friend code"));
}

void TestFriendRequestReachesTheServiceAndIsLogged() {
  Rig rig;
  rig.SignInWithFriends();
  CHECK(rig.service->AddFriend(" jklm-3456\r\n") == FriendRequestResult::kSent);
  CHECK(rig.CountLogs("DEBUG_SAULO online friend request sent code=JKLM-3456") == 1);
  CHECK(rig.Count("friends.add") == 1);
  rig.Reply("friends.add", "\"player\":\"JKLM-3456\",\"name\":\"Kim\",\"state\":\"pending\"");
  CHECK(rig.CountLogs("friend request accepted by service code=JKLM-3456 state=pending") == 1);

  CHECK(rig.service->AddFriend("NPQR-7892") == FriendRequestResult::kSent);
  rig.Reply("friends.add", "", "not_found");
  CHECK(rig.CountLogs("DEBUG_SAULO online friend request failed code=NPQR-7892 reason=not_found") == 1);

  CHECK(rig.service->AddFriend("nonsense") == FriendRequestResult::kInvalid);
  CHECK(rig.CountLogs("DEBUG_SAULO online friend request failed code=- reason=invalid") == 1);
  CHECK(rig.service->AddFriend(rig.me) == FriendRequestResult::kOwnCode);
  CHECK(rig.CountLogs("failed code=" + rig.me + " reason=own_code") == 1);
  for (const std::string& line : rig.logs) CHECK(!Contains(line, kToken));
}

void TestPendingAndReverseRequests() {
  Rig rig;
  rig.SignInWithFriends();
  CHECK(rig.service->AddFriend("JKLM-3456") == FriendRequestResult::kSent);
  rig.Reply("friends.add", "\"player\":\"JKLM-3456\",\"name\":\"Kim\",\"state\":\"pending\"");
  rig.Reply("friends.requests", "\"incoming\":[],\"outgoing\":[{\"player\":\"JKLM-3456\",\"name\":\"Kim\"}]");
  CHECK(rig.service->AddFriend("JKLM-3456") == FriendRequestResult::kAlreadyPending);
  CHECK(rig.service->snapshot().notice == "Friend request already pending.");
  CHECK(rig.Count("friends.add") == 1);
  CHECK(rig.CountLogs("failed code=JKLM-3456 reason=already_pending") == 1);

  rig.Feed("{\"ev\":\"friend.request\",\"player\":\"NPQR-7892\",\"name\":\"Zoe\"}");
  rig.Reply("friends.requests", "\"incoming\":[{\"player\":\"NPQR-7892\",\"name\":\"Zoe\"}],\"outgoing\":[]");
  CHECK(rig.service->AddFriend("NPQR-7892") == FriendRequestResult::kSent);
  rig.Reply("friends.add", "\"player\":\"NPQR-7892\",\"name\":\"Zoe\",\"state\":\"friends\"");
  CHECK(rig.service->snapshot().notice == "You are now friends.");
}

void TestFriendRequestNeedsAConnection() {
  Rig rig;
  CHECK(rig.service->AddFriend("JKLM-3456") == FriendRequestResult::kNotConnected);
  CHECK(rig.channels.empty());
  rig.Enable();
  rig.Tick();
  CHECK(rig.service->AddFriend("JKLM-3456") == FriendRequestResult::kNotConnected);
  CHECK(rig.Count("friends.add") == 0);
  CHECK(rig.service->snapshot().notice == "Not connected to the online service.");
}

void TestCopiedFriendCodeCanBePastedByAnotherInstance() {
  Rig a;
  const std::string token_b(64, 'b');
  Rig b(kGuestBase, false, token_b);
  a.SignIn("TestA");
  b.SignIn("TestB");
  const std::string copied = a.service->snapshot().friend_code;
  CHECK(copied == a.me && copied.size() == 9 && copied[4] == '-');
  CHECK(copied.find_first_of(" \t\r\n") == std::string::npos);
  CHECK(copied != b.service->snapshot().friend_code);
  CHECK(NormalizeFriendCode(copied) == copied);

  CHECK(b.service->AddFriend(copied + "\r\n") == FriendRequestResult::kSent);
  CHECK(Contains(b.LastRequest("friends.add"), "\"code\":\"" + copied + "\""));
  CHECK(a.service->AddFriend(copied) == FriendRequestResult::kOwnCode);
}

void TestFriendRequestsAreAnswered() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed(std::string("{\"ev\":\"friend.request\",\"player\":\"") + kFriendB + "\",\"name\":\"Zed\"}");
  CHECK(Contains(rig.service->snapshot().notice, "Zed wants to be your friend"));
  rig.Reply("friends.requests", std::string("\"incoming\":[{\"player\":\"") + kFriendB + "\",\"name\":\"Zed\"}],\"outgoing\":[]");
  CHECK(rig.service->snapshot().incoming.size() == 1);
  rig.service->AcceptFriend(kFriendB);
  CHECK(rig.service->snapshot().incoming.empty());
  CHECK(Contains(rig.LastRequest("friends.accept"), kFriendB));
  rig.service->RemoveFriend(kFriendA);
  CHECK(rig.service->snapshot().friends.size() == 1);
  CHECK(Contains(rig.LastRequest("friends.remove"), kFriendA));
  rig.service->DeclineFriend(kFriendB);
  CHECK(Contains(rig.LastRequest("friends.decline"), kFriendB));
}

void TestUnreachableServiceRetriesWithoutLosingState() {
  Rig rig;
  rig.Enable();
  rig.Tick();
  rig.Event(ChannelEvent::Kind::kClosed, "unreachable");
  CHECK(rig.service->snapshot().connection == Connection::kOffline);
  CHECK(rig.service->snapshot().connection_text == "Unable to reach online service.");
  size_t opened = rig.channels.size();
  rig.Tick(0.2);
  CHECK(rig.channels.size() == opened);
  for (int i = 0; i < 6; ++i) rig.Tick(10.0);
  CHECK(rig.channels.size() > opened);
}

void TestFatalSignInErrorsDoNotRetryAtOnce() {
  Rig rig;
  rig.Enable();
  rig.Tick();
  rig.Event(ChannelEvent::Kind::kOpen);
  rig.Reply("hello", "", "proto");
  CHECK(rig.service->snapshot().connection == Connection::kError);
  CHECK(Contains(rig.service->snapshot().connection_text, "different version"));
  const size_t opened = rig.channels.size();
  rig.Tick(10.0);
  CHECK(rig.channels.size() == opened);
}

void TestMalformedServerMessagesAreIgnored() {
  Rig rig;
  rig.SignInWithFriends();
  for (const char* text : {"", "not json", "[]", "{}", "{\"ev\":\"match.peer\"}", "{\"ev\":\"nope\"}",
                           "{\"re\":99,\"code\":\"ok\"}", "{\"ev\":\"party.update\",\"party\":{}}", "{\"ev\":\"invite\",\"invite\":\"x\"}",
                           "{\"ev\":\"friend.status\",\"friend\":{}}"}) {
    rig.Feed(text);
  }
  const Snapshot s = rig.service->snapshot();
  CHECK(s.connection == Connection::kOnline && s.friends.size() == 2 && !s.party && s.peers.empty());
}

void TestEveryConnectionReportCountsAsFinished() {
  Rig rig;
  rig.Enable();
  CHECK(rig.service->snapshot().tests_finished == 0);
  for (const char* verdict : {"Ready for online play", "Limited: the multiplayer UDP port could not be opened",
                              "Not ready: cannot reach the online service"}) {
    rig.test_result.verdict = verdict;
    rig.service->TestConnection();
    CHECK(rig.service->snapshot().testing || rig.service->snapshot().tests_finished > 0);
    rig.Tick();
    CHECK(!rig.service->snapshot().testing && rig.service->snapshot().test->verdict == verdict);
  }
  CHECK(rig.service->snapshot().tests_finished == 3);

  rig.test_throws = true;
  rig.service->TestConnection();
  rig.Tick();
  const Snapshot stopped = rig.service->snapshot();
  CHECK(!stopped.testing && stopped.tests_finished == 3);
  CHECK(rig.CountLogs("DEBUG_SAULO online connection test stopped without a report: test broke") == 1);

  // Online play off drops a test that is still running.
  Rig off;
  off.Enable();
  off.service->Apply({false, "Saulo"});
  off.service->TestConnection();
  off.Tick();
  CHECK(off.test_calls == 0 && off.service->snapshot().tests_finished == 0);
}

void TestConnectionTestRunsOnlyWhenEnabled() {
  Rig rig;
  rig.service->TestConnection();
  CHECK(rig.test_calls == 0);
  rig.Enable();
  rig.test_result.verdict = "Ready for online play";
  rig.test_result.ready = true;
  rig.test_result.latency_ms = 42;
  rig.test_result.details = {"service: ok", "STUN a: 1.2.3.4:5"};
  rig.service->TestConnection();
  rig.Tick();
  CHECK(rig.test_calls == 1);
  const Snapshot s = rig.service->snapshot();
  CHECK(s.test && s.test->latency_ms == 42 && s.test->verdict == "Ready for online play" && !s.testing);
  CHECK(rig.test_request.url == "wss://example.test/" && rig.test_request.name == "Saulo" &&
        rig.test_request.guest == "base" && rig.test_request.upnp &&
        rig.test_request.game_ports == (std::vector<uint16_t>{7001, 7002, 7003, 7004}));
  CHECK(rig.test_request.identity.player_id == rig.me && rig.test_request.stun == net::kDefaultStunServers);
  CHECK(rig.CountLogs("DEBUG_SAULO online connection test service: ok") == 1);
  CHECK(rig.CountLogs("DEBUG_SAULO online connection test Ready for online play") == 1);
  for (const std::string& line : rig.logs) CHECK(!Contains(line, kToken));
}

void TestTemporaryIdentityGetsASuffix() {
  Rig rig(kGuestBase, true);
  rig.Enable("Saulo");
  rig.Tick();
  rig.Event(ChannelEvent::Kind::kOpen);
  CHECK(Contains(rig.LastRequest("hello"), "\"name\":\"Saulo-2\""));
}

// ---- The party: invites into one hidden match, its room, and what is reported ----

constexpr char kMatch[] = "00ff00ff00ff00ff";
constexpr char kDiag[] = "0123456789ab";
constexpr char kJoinToken[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

std::string MemberJson(int slot, const char* player, const char* name, const char* state = "joined") {
  return "{\"slot\":" + std::to_string(slot) + ",\"player\":\"" + player + "\",\"name\":\"" + name +
         "\",\"platform\":\"Windows\",\"version\":\"0.2.2\",\"state\":\"" + state + "\"}";
}

std::string PartyJson(const std::vector<std::string>& members, const std::string& invited = "[]") {
  std::string list;
  for (size_t i = 0; i < members.size(); ++i) list += (i ? "," : "") + members[i];
  return std::string("{\"match\":\"") + kMatch + "\",\"diag\":\"" + kDiag + "\",\"host\":\"" + kFriendA +
         "\",\"generation\":1,\"session\":\"\",\"members\":[" + list + "],\"invited\":" + invited + "}";
}

std::string AccessBody(int slot, const std::string& party) {
  return std::string("\"match\":\"") + kMatch + "\",\"diag\":\"" + kDiag + "\",\"slot\":" + std::to_string(slot) +
         ",\"join_token\":\"" + kJoinToken + "\",\"generation\":0,\"party\":" + party;
}

// The service is signed in, a party exists and its room is open and greeted.
void InPartyAsHost(Rig& rig) {
  rig.SignInWithFriends();
  CHECK(rig.service->CreateParty());
  rig.Reply("party.create", AccessBody(1, PartyJson({MemberJson(1, rig.me.c_str(), "Saulo")})));
  rig.Tick();
}

void OpenRoom(Rig& rig, const std::string& members = "[]") {
  rig.Tick(0.1);
  CHECK(rig.channels.size() >= 2);
  rig.EventRoom(ChannelEvent::Kind::kOpen);
  const std::string hello = rig.LastRoomRequest("match.hello");
  CHECK(!hello.empty());
  rig.FeedRoom("{\"re\":" + std::to_string(SeqOf(hello)) + ",\"code\":\"ok\",\"generation\":1,\"relay_token\":4000000000,\"members\":" +
           members + "}");
}

void TestPartyIsCreatedByTheHostAndOpensItsRoom() {
  Rig rig;
  rig.SignInWithFriends();
  CHECK(rig.service->CreateParty());
  rig.Reply("party.create", AccessBody(1, PartyJson({MemberJson(1, rig.me.c_str(), "Saulo")})));
  const auto party = rig.service->party();
  CHECK(party && party->match == kMatch && party->diag == kDiag && party->members.size() == 1);
  CHECK(rig.service->my_slot() == 1);
  CHECK(!rig.service->CreateParty());  // already in one
  CHECK(rig.CountLogs(kJoinToken) == 0 && rig.CountLogs(std::string("token=") + kJoinToken) == 0);  // never logged

  // The room is a second connection to the match's own address, greeted with the private join token.
  rig.Tick(0.1);
  CHECK(rig.channels.size() == 2 && Contains(rig.channels[1]->url, std::string("/match/") + kMatch));
  rig.EventRoom(ChannelEvent::Kind::kOpen);
  const std::string hello = rig.LastRoomRequest("match.hello");
  CHECK(Contains(hello, std::string("\"match\":\"") + kMatch + "\"") && Contains(hello, "\"slot\":1") &&
        Contains(hello, std::string("\"token\":\"") + kJoinToken + "\"") && Contains(hello, "\"relay\":true") &&
        Contains(hello, "\"proto\":4"));
  CHECK(rig.service->snapshot().room_state == "connecting");
  rig.FeedRoom("{\"re\":" + std::to_string(SeqOf(hello)) + ",\"code\":\"ok\",\"generation\":1,\"relay_token\":4000000000,\"members\":[]}");
  CHECK(rig.service->snapshot().room_state == "online");
}

void TestThreeInvitesAreSentForTheSameParty() {
  Rig rig;
  InPartyAsHost(rig);
  for (const char* friend_code : {"BBBB-2222", "CCCC-3333", "DDDD-4444"}) {
    CHECK(rig.service->InviteToParty(friend_code));
    const std::string request = rig.LastRequest("invite.send");
    CHECK(Contains(request, std::string("\"player\":\"") + friend_code + "\""));
    CHECK(!Contains(request, "match") && !Contains(request, kMatch));  // the service knows the one party
    rig.Reply("invite.send", "\"invite\":\"i" + std::string(1, friend_code[0]) + "\"");
  }
  CHECK(rig.Count("invite.send") == 3);
  rig.Feed(std::string("{\"ev\":\"party.update\",\"party\":") +
           PartyJson({MemberJson(1, rig.me.c_str(), "Saulo", "connected")},
                     "[{\"player\":\"BBBB-2222\",\"name\":\"B\"},{\"player\":\"CCCC-3333\",\"name\":\"C\"},{\"player\":\"DDDD-4444\",\"name\":\"D\"}]") +
           "}");
  const Snapshot state = rig.service->snapshot();
  CHECK(state.party && state.party->invited.size() == 3);
  int invited_cards = 0;
  for (const PartySlotView& slot : DescribePartySlots(state)) invited_cards += slot.invited ? 1 : 0;
  CHECK(invited_cards == 3);

}

void TestInviteErrorsAreExplainedAndReported() {
  Rig rig;
  InPartyAsHost(rig);
  const struct {
    const char* code;
    const char* text;
  } cases[] = {{"party_full", "The party is full."}, {"already_invited", "That friend already has an invite."},
               {"not_friends", "That player is not your friend."}, {"offline", "Your friend is not online."},
               {"version_mismatch", "Game versions do not match."}, {"busy", "One of you is already in a match."}};
  for (const auto& entry : cases) {
    CHECK(rig.service->InviteToParty("BBBB-2222"));
    rig.Reply("invite.send", {}, entry.code);
    CHECK(rig.service->snapshot().notice == entry.text);
  }
  rig.Feed("{\"re\":1,\"code\":\"ok\"}");
}

void TestInviteIsAcceptedAndJoinsTheHiddenMatch() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"},\"expires_ms\":120000}");
  const Snapshot offered = rig.service->snapshot();
  CHECK(offered.invites.size() == 1 && offered.invites[0].id == "i1" && offered.invites[0].from_name == "SauloB");
  CHECK(Contains(offered.notice, "invited you to a party"));
  CHECK(!rig.service->AcceptPartyInvite("nope"));
  CHECK(rig.service->AcceptPartyInvite("i1"));
  CHECK(Contains(rig.LastRequest("invite.accept"), "\"invite\":\"i1\""));
  rig.Reply("invite.accept", AccessBody(3, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"), MemberJson(2, kFriendB, "Zed"),
                                                      MemberJson(3, rig.me.c_str(), "Saulo")})));
  CHECK(rig.service->my_slot() == 3 && rig.service->party()->members.size() == 3);
  CHECK(rig.service->Invites().empty());
  OpenRoom(rig);
  const auto peers = rig.service->Peers();
  CHECK(peers.size() == 2 && peers[0].slot == 1 && peers[0].host && peers[1].slot == 2 && peers[0].player == kFriendA);
  CHECK(peers[0].match == kMatch);
}

std::string InviteWithSession(const char* id, const char* session) {
  return std::string("{\"ev\":\"invite\",\"invite\":\"") + id + "\",\"from\":{\"player\":\"" + kFriendA +
         "\",\"name\":\"SauloB\"},\"session\":\"" + session + "\"}";
}

void AcceptInviteIntoParty(Rig& rig, const char* id) {
  CHECK(rig.service->AcceptPartyInvite(id));
  rig.Reply("invite.accept", AccessBody(3, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"), MemberJson(2, kFriendB, "Zed"),
                                                      MemberJson(3, rig.me.c_str(), "Saulo")})));
}

void TestAcceptedInviteIsDeliveredOnceItsPartyIsJoinedAndJoinsOnce() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed(InviteWithSession("i1", kSession));
  CHECK(rig.service->Invites().size() == 1 && rig.service->Invites()[0].session == kSession);
  CHECK(rig.service->AcceptPartyInvite("i1"));
  CHECK(!rig.service->AcceptedInviteInfo() && rig.delivered.empty());  // not before the party is joined
  rig.Reply("invite.accept", AccessBody(3, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"), MemberJson(2, kFriendB, "Zed"),
                                                      MemberJson(3, rig.me.c_str(), "Saulo")})));
  CHECK(rig.delivered.size() == 1 && rig.delivered[0].generation == 1 && rig.delivered[0].session == kSession &&
        rig.delivered[0].host == kFriendA && rig.delivered[0].host_name == "SauloB");
  const auto accepted = rig.service->AcceptedInviteInfo();
  CHECK(accepted && accepted->session == kSession && accepted->ready);
  const auto results = rig.service->PlayerMatchResults();
  CHECK(results.size() == 1 && results[0].session == kSession && results[0].player == kFriendA);
  CHECK(rig.service->JoinPlayerMatch(kSession));
  CHECK(Contains(rig.LastRequest("pm.join"), std::string("\"session\":\"") + kSession + "\""));
  CHECK(!rig.service->JoinPlayerMatch(kSession));  // one join at a time
  rig.Reply("pm.join", AccessBody(3, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"), MemberJson(3, rig.me.c_str(), "Saulo")})));
  CHECK(!rig.service->AcceptedInviteInfo() && rig.service->PlayerMatchResults().empty());
  CHECK(rig.service->party().has_value() && rig.delivered.size() == 1);
}

void TestInviteWithoutAHostedSessionOnlyJoinsTheParty() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"},\"session\":\"\"}");
  AcceptInviteIntoParty(rig, "i1");
  CHECK(rig.delivered.empty() && !rig.service->AcceptedInviteInfo() && rig.service->party().has_value());
  CHECK(!rig.service->JoinPlayerMatch(kSession));
}

void TestRejectedInviteAcceptAndRejectedJoinEndOnlyThatInvite() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed(InviteWithSession("i1", kSession));
  CHECK(rig.service->AcceptPartyInvite("i1"));
  rig.Reply("invite.accept", {}, "party_full");
  CHECK(rig.delivered.empty() && !rig.service->AcceptedInviteInfo() && !rig.service->party());
  CHECK(rig.CountLogs("accepted invite dropped generation=1 reason=accept_rejected_party_full") == 1);

  rig.Feed(InviteWithSession("i2", kSession));
  AcceptInviteIntoParty(rig, "i2");
  CHECK(rig.delivered.size() == 1 && rig.delivered[0].generation == 2);
  CHECK(rig.service->JoinPlayerMatch(kSession));
  rig.Reply("pm.join", {}, "not_found");
  CHECK(!rig.service->AcceptedInviteInfo() && rig.service->party().has_value());  // the party stays
  CHECK(!rig.service->JoinPlayerMatch(kSession));
}

void TestEndedOrReplacedInviteFailsItsGeneration() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed(InviteWithSession("i1", kSession));
  CHECK(rig.service->AcceptPartyInvite("i1"));
  rig.Feed("{\"ev\":\"invite.ended\",\"invite\":\"i1\",\"reason\":\"expired\"}");
  CHECK(!rig.service->AcceptedInviteInfo());
  CHECK(rig.CountLogs("accepted invite dropped generation=1 reason=invite_ended_expired") == 1);

  Rig left;
  left.SignInWithFriends();
  left.Feed(InviteWithSession("i1", kSession));
  AcceptInviteIntoParty(left, "i1");
  CHECK(left.service->AcceptedInviteInfo().has_value());
  left.Feed(std::string("{\"ev\":\"party.ended\",\"match\":\"") + kMatch + "\",\"reason\":\"host_left\"}");
  CHECK(!left.service->AcceptedInviteInfo() && !left.service->JoinPlayerMatch(kSession));
  CHECK(left.CountLogs("accepted invite dropped generation=1 reason=party_ended_host_left") == 1);

  Rig declined;
  declined.SignInWithFriends();
  declined.Feed(InviteWithSession("i1", kSession));
  AcceptInviteIntoParty(declined, "i1");
  declined.service->DropAcceptedInvite("declined");
  CHECK(!declined.service->AcceptedInviteInfo() && !declined.service->JoinPlayerMatch(kSession));
}

void TestInviteRefusalsAndEndings() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.service->DeclinePartyInvite("i1");
  CHECK(rig.service->Invites().empty() && Contains(rig.LastRequest("invite.decline"), "\"invite\":\"i1\""));

  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i2\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.Feed("{\"ev\":\"invite.ended\",\"invite\":\"i2\",\"reason\":\"expired\"}");
  CHECK(rig.service->Invites().empty());

  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i3\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  CHECK(rig.service->AcceptPartyInvite("i3"));
  rig.Reply("invite.accept", {}, "party_full");
  CHECK(rig.service->Invites().empty() && !rig.service->party() && rig.service->snapshot().notice == "The party is full.");
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i4\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  CHECK(rig.service->AcceptPartyInvite("i4"));
  rig.Reply("invite.accept", {}, "host_left");
  CHECK(rig.service->snapshot().notice == "The host has already left.");
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i5\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  CHECK(rig.service->AcceptPartyInvite("i5"));
  rig.Reply("invite.accept", {}, "expired");
  CHECK(rig.service->snapshot().notice == "The invite has expired.");
  CHECK(rig.channels.size() == 1);  // no room was ever opened
}

void TestRoomHelloThatRacesTheRosterIsRetried() {
  Rig rig;
  InPartyAsHost(rig);
  rig.Tick(0.1);
  rig.EventRoom(ChannelEvent::Kind::kOpen);
  rig.FeedRoom("{\"re\":" + std::to_string(SeqOf(rig.LastRoomRequest("match.hello"))) + ",\"code\":\"not_found\"}");
  CHECK(rig.service->party().has_value());  // not given up on the first refusal
  CHECK(rig.channels.size() == 2);
  rig.Tick(2.5);
  CHECK(rig.channels.size() == 3);  // a new room connection
  rig.EventRoom(ChannelEvent::Kind::kOpen);
  rig.FeedRoom("{\"re\":" + std::to_string(SeqOf(rig.LastRoomRequest("match.hello"))) + ",\"code\":\"ok\",\"generation\":1,\"relay_token\":7,\"members\":[]}");
  CHECK(rig.service->snapshot().room_state == "online");

  // A refusal that will not change ends the party.
  Rig denied;
  InPartyAsHost(denied);
  denied.Tick(0.1);
  denied.EventRoom(ChannelEvent::Kind::kOpen);
  denied.FeedRoom("{\"re\":" + std::to_string(SeqOf(denied.LastRoomRequest("match.hello"))) + ",\"code\":\"unauthorized\"}");
  CHECK(!denied.service->party().has_value());
}

void TestPartyEndsWhenTheServiceSaysSo() {
  Rig rig;
  InPartyAsHost(rig);
  OpenRoom(rig);
  rig.Feed(std::string("{\"ev\":\"party.ended\",\"match\":\"") + kMatch + "\",\"reason\":\"host_left\"}");
  CHECK(!rig.service->party() && rig.service->my_slot() == 0 && rig.service->snapshot().room_state == "none");
  CHECK(rig.service->snapshot().notice == "The host left the party.");
  CHECK(rig.service->Peers().empty());
  CHECK(rig.Room().closed);  // the room connection is dropped

  // An event of another match does not touch this party.
  Rig other;
  InPartyAsHost(other);
  other.Feed("{\"ev\":\"party.ended\",\"match\":\"ffffffffffffffff\",\"reason\":\"expired\"}");
  CHECK(other.service->party().has_value());
  // Being left out of the roster means being removed.
  other.Feed(std::string("{\"ev\":\"party.update\",\"party\":") + PartyJson({MemberJson(2, kFriendB, "Zed")}) + "}");
  CHECK(!other.service->party().has_value() && other.service->snapshot().notice == "You were removed from the party.");
}

void TestLeavingAndTurningOnlineOffLeavesTheParty() {
  Rig rig;
  InPartyAsHost(rig);
  OpenRoom(rig);
  rig.service->LeaveParty();
  CHECK(rig.Count("party.leave") == 1 && !rig.service->party());
  CHECK(rig.channels[1]->closed);

  Rig off;
  InPartyAsHost(off);
  OpenRoom(off);
  off.service->Apply({false, "Saulo"});
  CHECK(!off.service->party() && off.channels[1]->closed);
  CHECK(off.service->snapshot().peers.empty() && off.service->snapshot().connection == Connection::kOff);
}

void TestRoomLossReconnectsWithItsGeneration() {
  Rig rig;
  InPartyAsHost(rig);
  OpenRoom(rig);
  const size_t before = rig.channels.size();
  rig.EventRoom(ChannelEvent::Kind::kClosed, "closed");
  CHECK(rig.service->party().has_value() && rig.service->snapshot().room_state == "connecting");
  rig.Tick(1.5);
  CHECK(rig.channels.size() == before + 1);
  rig.EventRoom(ChannelEvent::Kind::kOpen);
  const std::string hello = rig.LastRoomRequest("match.hello");
  CHECK(Contains(hello, "\"generation_seen\":1") && Contains(hello, "\"slot\":1"));
}

void TestSlotsAreStableAndShownForEveryMember() {
  Rig rig;
  InPartyAsHost(rig);
  rig.Feed(std::string("{\"ev\":\"party.update\",\"party\":") +
           PartyJson({MemberJson(1, rig.me.c_str(), "Saulo", "connected"), MemberJson(2, kFriendB, "Zed", "connected"),
                      MemberJson(4, "CCCC-3333", "Cee", "joined")}) +
           "}");
  const std::vector<PartySlotView> slots = DescribePartySlots(rig.service->snapshot());
  CHECK(slots.size() == 4);
  CHECK(slots[0].occupied && slots[0].host && slots[0].you && slots[0].name == "Saulo");
  CHECK(slots[1].occupied && !slots[1].host && !slots[1].you && slots[1].name == "Zed");
  CHECK(!slots[2].occupied && slots[2].status == "Empty");  // slot 3 stays free: slots are stable
  CHECK(slots[3].occupied && slots[3].name == "Cee" && slots[3].status == "Joining");
  CHECK(PartyHeadline(rig.service->snapshot()) == "Party 3/4");
  CHECK(DescribePartySlots(Snapshot{}).size() == 4 && PartyHeadline(Snapshot{}).empty());
  const auto peers = rig.service->Peers();
  CHECK(peers.size() == 2 && peers[0].slot == 2 && peers[1].slot == 4);
}

void TestGameSessionAdvertisementUsesTheParty() {
  Rig rig;
  rig.SignInWithFriends();
  rig.service->HostPlayerMatch({}, 4);
  CHECK(Contains(rig.LastRequest("pm.host"), "\"slots\":4"));
  // The advertisement made the party when there was none; its answer carries the access.
  rig.Reply("pm.host", std::string("\"session\":\"") + kSession + "\"," + AccessBody(1, PartyJson({MemberJson(1, rig.me.c_str(), "Saulo")})));
  CHECK(rig.service->party() && rig.service->snapshot().player_match.session == kSession && rig.service->my_slot() == 1);
  rig.service->HostPlayerMatch({}, 9);  // never above four
  CHECK(Contains(rig.LastRequest("pm.host"), "\"slots\":4"));
  rig.service->HostPlayerMatch({}, 1);  // never below two
  CHECK(Contains(rig.LastRequest("pm.host"), "\"slots\":2"));

  rig.service->SetPlayerMatchSearch(true);
  rig.Tick(3.5);
  rig.Reply("pm.search", SearchAnswerFor(4));
  CHECK(rig.service->PlayerMatchResults().size() == 1 && rig.service->PlayerMatchResults()[0].slots == 4);
  CHECK(rig.service->JoinPlayerMatch(kSession));
  CHECK(Contains(rig.LastRequest("pm.join"), std::string("\"session\":\"") + kSession + "\""));
  CHECK(!rig.service->JoinPlayerMatch("ffffffffffffffff"));
}

void TestGuestJoinsAFriendsHostedSessionWithoutAParty() {
  Rig rig;
  rig.SignInWithFriends();
  CHECK(!rig.service->party().has_value());
  rig.service->SetPlayerMatchSearch(true);
  rig.Tick(3.5);
  rig.Reply("pm.search", "\"sessions\":[]");
  CHECK(rig.service->PlayerMatchResults().empty());
  rig.Tick(3.5);
  rig.Reply("pm.search", SearchAnswerFor(4));
  CHECK(rig.service->PlayerMatchResults().size() == 1);
  CHECK(rig.service->JoinPlayerMatch(kSession));
  rig.Reply("pm.join", std::string("\"session\":\"") + kSession + "\"," +
                           AccessBody(2, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"), MemberJson(2, rig.me.c_str(), "Saulo")})));
  CHECK(rig.service->party().has_value() && rig.service->my_slot() == 2 && rig.service->party()->members.size() == 2);
  CHECK(rig.service->Peers().size() == 1 && rig.service->Peers()[0].host);
}

void TestHostedSessionStaysAdvertisedUntilTheGameDeletesIt() {
  Rig rig;
  rig.SignInWithFriends();
  rig.service->HostPlayerMatch({}, 4);
  rig.Reply("pm.host", std::string("\"session\":\"") + kSession + "\"," + AccessBody(1, PartyJson({MemberJson(1, rig.me.c_str(), "Saulo")})));
  for (int i = 0; i < 40; ++i) rig.Tick(5);
  CHECK(rig.Count("pm.cancel") == 0 && rig.service->snapshot().player_match.hosting);
  CHECK(rig.service->snapshot().player_match.session == kSession && rig.service->party().has_value());

  // A guest searching now sees it; the host's party grows to four as guests join.
  const std::string one = MemberJson(1, rig.me.c_str(), "Saulo", "connected");
  rig.Feed("{\"ev\":\"party.update\",\"party\":" + PartyJson({one, MemberJson(2, kFriendA, "SauloB")}) + "}");
  CHECK(rig.service->party()->members.size() == 2 && rig.service->Peers().size() == 1);
  rig.Feed("{\"ev\":\"party.update\",\"party\":" +
           PartyJson({one, MemberJson(2, kFriendA, "SauloB"), MemberJson(3, kFriendB, "Zed"), MemberJson(4, "WXYZ-2345", "Four")}) + "}");
  CHECK(rig.service->party()->members.size() == 4 && rig.service->Peers().size() == 3);

  rig.service->CancelPlayerMatch();
  CHECK(rig.Count("pm.cancel") == 1 && !rig.service->snapshot().player_match.hosting);
  CHECK(rig.service->party().has_value());
}

std::vector<std::string> Batches(Rig& rig) {
  std::vector<std::string> out;
  for (const std::shared_ptr<ChannelState>& channel : rig.channels) {
    for (const std::string& sent : channel->sent) {
      if (Contains(sent, "\"op\":\"telemetry.batch\"")) out.push_back(sent);
    }
  }
  return out;
}

std::vector<int> EventNumbers(const std::string& batch) {
  std::vector<int> out;
  const std::regex pattern("\"e\":\"[a-z_]+\",\"n\":([0-9]+)");
  for (std::sregex_iterator it(batch.begin(), batch.end(), pattern), end; it != end; ++it) {
    out.push_back(std::stoi((*it)[1]));
  }
  return out;
}

void TestTelemetryIsBatchedAndNeverCarriesSecrets() {
  Rig rig;
  InPartyAsHost(rig);
  OpenRoom(rig);
  rig.service->InviteToParty("BBBB-2222");
  rig.Reply("invite.send", {}, "offline");
  rig.Tick(20);
  const std::vector<std::string> sent = Batches(rig);
  CHECK(sent.size() == 1);
  const std::string batch = sent.empty() ? std::string() : sent[0];
  CHECK(Contains(batch, "\"seq\":") && Contains(batch, std::string("\"diag\":\"") + kDiag + "\""));
  CHECK(Contains(batch, "\"slot\":1") && Contains(batch, "\"invite_failed\"") && Contains(batch, "\"reason\":\"offline\""));
  CHECK(!Contains(batch, "\"party_created\"") && !Contains(batch, "\"invite_sent\""));
  for (const std::string& secret : {std::string(kJoinToken), std::string(kToken), std::string(rig.me), std::string(kFriendA),
                                    std::string("BBBB-2222"), std::string("Saulo")}) {
    CHECK(!Contains(batch, secret));
  }
  rig.Tick(1);
  CHECK(Batches(rig).size() == 1);  // a batch per interval, not one request per event

  rig.Reply("telemetry.batch");
  rig.service->InviteToParty("BBBB-2222");
  rig.Reply("invite.send", {}, "not_friends");
  rig.Tick(20);
  CHECK(Batches(rig).size() == 2);

  // The service never waits for telemetry: a refusal changes nothing.
  rig.Reply("telemetry.batch", {}, "limit");
  CHECK(rig.service->party().has_value() && rig.service->SignedIn());
}

void TestEventsBeforeAnyPartyUseTheLobbyContext() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"},\"expires_ms\":120000}");
  rig.Tick(20);
  std::vector<std::string> sent = Batches(rig);
  CHECK(sent.size() == 1);
  const std::string lobby = sent.empty() ? std::string() : sent[0];
  CHECK(Contains(lobby, "\"invite_received\"") && Contains(lobby, "\"slot\":0") && Contains(lobby, "\"party\":0"));
  CHECK(!Contains(lobby, std::string("\"diag\":\"") + kDiag + "\""));
  CHECK(!Contains(lobby, "\"session\":") && !Contains(lobby, "\"peers\":"));
  rig.Reply("telemetry.batch");

  CHECK(rig.service->AcceptPartyInvite("i1"));
  rig.Reply("invite.accept", AccessBody(2, PartyJson({MemberJson(1, kFriendA, "SauloB", "connected"),
                                                      MemberJson(2, rig.me.c_str(), "Saulo")})));
  rig.Reply("telemetry.batch");
  rig.Tick(20);
  sent = Batches(rig);
  CHECK(sent.size() >= 2);
  const std::string party = sent.empty() ? std::string() : sent.back();
  CHECK(Contains(party, std::string("\"diag\":\"") + kDiag + "\"") && Contains(party, "\"slot\":2"));
  CHECK(Contains(party, "\"join_started\"") || Contains(party, "\"join_completed\""));
  for (const std::string& batch : sent) {
    CHECK(!Contains(batch, kFriendA) && !Contains(batch, rig.me) && !Contains(batch, kJoinToken));
  }

  rig.Reply("telemetry.batch");
  rig.Feed("{\"ev\":\"party.ended\",\"match\":\"" + std::string(kMatch) + "\",\"reason\":\"host_left\"}");
  const std::vector<std::string> after = Batches(rig);
  CHECK(after.size() > sent.size());
  CHECK(Contains(after.back(), "\"session_end\"") && Contains(after.back(), "\"reason\":\"host_left\""));
  rig.Reply("telemetry.batch");
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i2\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.Tick(20);
  const std::vector<std::string> last = Batches(rig);
  CHECK(Contains(last.back(), "\"slot\":0") && !Contains(last.back(), "\"session\":") &&
        !Contains(last.back(), std::string("\"diag\":\"") + kDiag + "\""));
}

void TestARetriedBatchKeepsItsEventNumbers() {
  Rig rig;
  rig.SignInWithFriends();
  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.Tick(20);
  std::vector<std::string> sent = Batches(rig);
  CHECK(sent.size() == 1);
  const std::vector<int> first = EventNumbers(sent[0]);
  CHECK(first.size() == 1);

  rig.Reply("telemetry.batch", {}, "limit");
  rig.Tick(20);
  sent = Batches(rig);
  CHECK(sent.size() == 2 && EventNumbers(sent[1]) == first && sent[1] != sent[0]);
  rig.Reply("telemetry.batch");

  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i2\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.Tick(20);
  sent = Batches(rig);
  CHECK(sent.size() == 3);
  const std::vector<int> second = EventNumbers(sent[2]);
  CHECK(second.size() == 1 && second[0] > first[0]);

  rig.Event(ChannelEvent::Kind::kClosed, "closed");
  for (int i = 0; i < 20 && rig.channels.size() < 2; ++i) rig.Tick(5);
  CHECK(rig.channels.size() == 2);
  rig.Event(ChannelEvent::Kind::kOpen);
  rig.Reply("hello", "\"player\":\"" + rig.me + "\",\"name\":\"Saulo\",\"friend_code\":\"" + rig.me + "\"");
  rig.Tick(20);
  sent = Batches(rig);
  CHECK(sent.size() == 4);
  const std::vector<int> resent = sent.size() == 4 ? EventNumbers(sent[3]) : std::vector<int>();
  CHECK(resent.size() == 2 && resent[0] == second[0] && resent[1] > second[0]);  // the reconnect adds its own event
  rig.Reply("telemetry.batch");

  rig.Feed("{\"ev\":\"invite\",\"invite\":\"i3\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"SauloB\"}}");
  rig.Tick(20);
  rig.Tick(25);  // never answered: it is sent again
  sent = Batches(rig);
  CHECK(sent.size() == 6 && EventNumbers(sent[4]) == EventNumbers(sent[5]) && !EventNumbers(sent[4]).empty());
  rig.Reply("telemetry.batch", {}, "limit");  // refused after the retry: the events are dropped for good
  rig.Tick(20);
  sent = Batches(rig);
  CHECK(sent.size() == 7 && EventNumbers(sent[6]).empty() && Contains(sent[6], "\"dropped\":1"));
}

void TestConnectionTestSendsOneBeaconWithoutSecrets() {
  Rig rig;
  rig.SignIn();
  rig.test_result.worker_status = "ok";
  rig.test_result.ws_status = "ok";
  rig.test_result.stun_status = "failed";
  rig.test_result.router_kind = "none";
  rig.test_result.worker_reachable = true;
  rig.test_result.latency_ms = 42;
  rig.test_result.service_protocol = 4;
  rig.test_result.failure = "stun_failed";
  rig.service->TestConnection();
  rig.Tick();
  CHECK(rig.beacons.size() == 1);
  const std::string path = rig.beacons.empty() ? std::string() : rig.beacons[0];
  CHECK(path.rfind("/telemetry/test?diag=", 0) == 0);
  for (const char* part : {"&plat=Windows", "&proto=4", "&sproto=4", "&worker=ok", "&ws=ok", "&ms=42", "&stun=failed", "&usable=0",
                           "&fail=stun_failed", "&router=none", "&direct=0", "&relay=0"}) {
    CHECK(Contains(path, part));
  }
  for (const std::string& secret : {std::string(kToken), std::string(rig.me), std::string(kFriendA), std::string("Saulo"),
                                    std::string("example.test"), std::string("127.0.0.1")}) {
    CHECK(!Contains(path, secret));
  }
  CHECK(path.find(' ') == std::string::npos && path.find('/', 16) == std::string::npos);

  rig.beacon_throws = true;
  rig.test_result.failure.clear();
  rig.test_result.overall_usable = true;
  rig.service->TestConnection();
  rig.Tick();
  CHECK(rig.beacons.size() == 2 && !Contains(rig.beacons[1], "&fail=") && Contains(rig.beacons[1], "&usable=1"));
  CHECK(rig.service->snapshot().test.has_value() && !rig.service->snapshot().testing);
}

void TestConnectionTestBeaconNeedsNoSessionOrConnection() {
  Rig rig;
  rig.Enable();
  rig.test_result.worker_status = "unreachable";
  rig.test_result.ws_status = "skipped";
  rig.test_result.failure = "worker_unreachable";
  rig.service->TestConnection();
  rig.Tick();
  CHECK(rig.beacons.size() == 1 && Contains(rig.beacons[0], "&worker=unreachable") && Contains(rig.beacons[0], "&ws=skipped"));
  CHECK(Contains(rig.beacons[0], "&fail=worker_unreachable") && Batches(rig).empty());
}

void TestEveryReportedReasonIsAKnownCode() {
  Rig rig;
  InPartyAsHost(rig);
  OpenRoom(rig);
  rig.service->InviteToParty("BBBB-2222");
  rig.Reply("invite.send", {}, "version_mismatch");
  rig.service->LeaveParty();
  rig.Tick(20);
  std::string all;
  for (const std::string& batch : Batches(rig)) all += batch;
  CHECK(Contains(all, "\"session_end\"") && Contains(all, "\"reason\":\"left\""));
  size_t found = 0;
  const std::regex reasons("\"reason\":\"([a-z_]+)\"");
  for (std::sregex_iterator it(all.begin(), all.end(), reasons), end; it != end; ++it) {
    const std::string code = (*it)[1];
    ++found;
    CHECK(code != "other" && !ReasonCategory(code).empty());
  }
  CHECK(found >= 2);

  const char* endings[] = {"host_left", "kicked", "expired", "closed"};
  for (const char* ending : endings) {
    Rig other;
    InPartyAsHost(other);
    other.Feed(std::string("{\"ev\":\"party.ended\",\"match\":\"") + kMatch + "\",\"reason\":\"" + ending + "\"}");
    other.Tick(20);
    std::string text;
    for (const std::string& batch : Batches(other)) text += batch;
    CHECK(Contains(text, std::string("\"reason\":\"") + ending + "\""));
  }
  for (const char* refusal : {"unauthorized", "not_found", "party_full"}) {
    CHECK(!ReasonCategory(refusal).empty());
  }
}

void TestOnlineOffCreatesNoPartyOrRoom() {
  Rig rig;
  CHECK(!rig.service->CreateParty() && !rig.service->InviteToParty("BBBB-2222") && !rig.service->AcceptPartyInvite("i"));
  rig.service->LeaveParty();
  rig.service->KickFromParty("BBBB-2222");
  CHECK(rig.channels.empty() && !rig.service->party() && rig.service->Peers().empty() && rig.service->Assignments().empty());
}

}  // namespace

int main() {
  std::printf("online service tests\n");
  TestOnlineOffHasNoNetworkBehaviour();
  TestSignInSendsPresenceDetailsAndNeverTheTokenInLogs();
  TestFriendsAndPresenceAreShown();
  TestAddFriendValidatesBeforeSending();
  TestFriendRequestReachesTheServiceAndIsLogged();
  TestPendingAndReverseRequests();
  TestFriendRequestNeedsAConnection();
  TestCopiedFriendCodeCanBePastedByAnotherInstance();
  TestFriendRequestsAreAnswered();
  TestUnreachableServiceRetriesWithoutLosingState();
  TestFatalSignInErrorsDoNotRetryAtOnce();
  TestMalformedServerMessagesAreIgnored();
  TestEveryConnectionReportCountsAsFinished();
  TestConnectionTestRunsOnlyWhenEnabled();
  TestTemporaryIdentityGetsASuffix();
  TestPartyIsCreatedByTheHostAndOpensItsRoom();
  TestThreeInvitesAreSentForTheSameParty();
  TestInviteErrorsAreExplainedAndReported();
  TestInviteIsAcceptedAndJoinsTheHiddenMatch();
  TestAcceptedInviteIsDeliveredOnceItsPartyIsJoinedAndJoinsOnce();
  TestInviteWithoutAHostedSessionOnlyJoinsTheParty();
  TestRejectedInviteAcceptAndRejectedJoinEndOnlyThatInvite();
  TestEndedOrReplacedInviteFailsItsGeneration();
  TestInviteRefusalsAndEndings();
  TestRoomHelloThatRacesTheRosterIsRetried();
  TestPartyEndsWhenTheServiceSaysSo();
  TestLeavingAndTurningOnlineOffLeavesTheParty();
  TestRoomLossReconnectsWithItsGeneration();
  TestSlotsAreStableAndShownForEveryMember();
  TestGameSessionAdvertisementUsesTheParty();
  TestGuestJoinsAFriendsHostedSessionWithoutAParty();
  TestHostedSessionStaysAdvertisedUntilTheGameDeletesIt();
  TestTelemetryIsBatchedAndNeverCarriesSecrets();
  TestEventsBeforeAnyPartyUseTheLobbyContext();
  TestARetriedBatchKeepsItsEventNumbers();
  TestConnectionTestSendsOneBeaconWithoutSecrets();
  TestConnectionTestBeaconNeedsNoSessionOrConnection();
  TestEveryReportedReasonIsAKnownCode();
  TestOnlineOffCreatesNoPartyOrRoom();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
