#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "online/connection_test.h"
#include "online/identity.h"
#include "online/lobby_link.h"
#include "online/match_session.h"
#include "online/protocol.h"
#include "online/telemetry.h"

namespace tmnt::online {

inline constexpr char kDefaultServiceUrl[] = "wss://tmnt-lobby.saulobenigno.workers.dev/";

struct ServiceConfig {
  std::string url = kDefaultServiceUrl;
  std::string guest = kGuestBase;
  int build = kOnlineBuild;
  std::string version;   // the port version friends see
  std::string platform;  // Windows or Linux
  std::vector<uint16_t> game_ports = {7001, 7002, 7003, 7004};
  std::vector<std::string> lan;  // this PC's own addresses, offered to a peer on the same network
  std::function<Identity()> identity;
  std::function<void(const std::string&)> log;
  ChannelFactory open_channel = OpenLobbyLink;
  std::string stun = net::kDefaultStunServers;
  bool upnp = true;
  std::function<ConnectionReport(const ConnectionTestRequest&)> test = RunConnectionTest;
  // The early-bound gameplay socket. Every match learns its public endpoint from it and proves each peer's endpoint
  // over it.
  netplay::OnlineGameSocket* game_socket = nullptr;
  // The STUN servers of `stun` as addresses; the default resolves them through DNS.
  std::function<std::vector<netplay::Endpoint>(const std::string& stun)> resolve_stun;
  // Tests run the connection test and the STUN lookup inline instead of on a thread.
  bool synchronous_probes = false;
  // Tells the service this client can play through its relay (with a gameplay socket only).
  bool relay = true;
  // Every pair of participants connects (the game's own topology); false: only pairs with the host.
  bool full_mesh = true;
  // Called with each operational event of a party and its matches (best effort; must not block).
  std::function<void(const SessionEvent&)> event;
  // The game image's short fingerprint for telemetry; empty when unknown.
  std::string fingerprint;
  // Receives the path of the support beacon for each finished Connection Test (best effort; must not block). The
  // default fetches it from the service on a detached thread.
  std::function<void(const std::string& path)> test_beacon;
  // Called once the party of an accepted invite that names a game session is joined.
  std::function<void(const AcceptedInvite&)> invite_accepted;
};

struct UserSettings {
  bool enabled = false;
  std::string name = kDefaultOnlineName;
};

// The game-session advertisement this player is making or looking for, driven by the game's own menus.
struct PlayerMatchSnapshot {
  bool hosting = false;
  std::string session;  // the advertisement, once the service has answered
  bool searching = false;
  std::vector<PlayerMatchAd> results;
  int search_generation = 0;  // grows with every answered search
};

struct Snapshot {
  Connection connection = Connection::kOff;
  std::string connection_text;
  std::string friend_code;
  bool fetched = false;
  std::vector<Friend> friends;
  std::vector<Request> incoming;  // friend requests to answer
  std::vector<Request> outgoing;  // friend requests still waiting
  std::vector<Invite> invites;    // friends' invitations to their party, oldest first
  PlayerMatchSnapshot player_match;
  std::optional<Party> party;
  int my_slot = 0;
  std::string room_state;  // none, connecting, online
  std::vector<PeerView> peers;
  std::string notice;  // the last thing worth telling the player; stays until replaced
  bool testing = false;
  std::optional<ConnectionReport> test;
  int tests_finished = 0;  // Connection tests that produced a report; one that stopped without one is not counted
};

// Who this player is on the service right now, for the game's title-local LIVE profile. Never the token.
struct OnlineProfile {
  bool signed_in = false;
  std::string friend_code;
  std::string name;
};

// A remote participant of the current party, as the game and its bridge see it.
struct ActivePeer {
  std::string match;
  int slot = 0;
  std::string player;  // friend code
  std::string name;
  bool host = false;
};

// What AddFriend did with the text it was given; only kSent reaches the service.
enum class FriendRequestResult { kSent, kNotConnected, kInvalid, kOwnCode, kAlreadyFriends, kAlreadyPending };

// The player's connection to the online service: identity, friends, presence, invites and the party (up to four players
// over one hidden match), plus the match room that carries each pair's Direct or Relay establishment. Game traffic never
// passes through it except on a pair's relay. Thread-safe.
class OnlineService {
 public:
  explicit OnlineService(ServiceConfig config);
  ~OnlineService();
  OnlineService(const OnlineService&) = delete;
  OnlineService& operator=(const OnlineService&) = delete;

  void SetGuest(const std::string& guest, int build);
  void Apply(const UserSettings& settings);
  // Every frame, from whichever thread is presenting.
  void Update(double now);

  FriendRequestResult AddFriend(const std::string& code);
  void AcceptFriend(const std::string& player);
  void DeclineFriend(const std::string& player);  // also withdraws an outgoing request
  void RemoveFriend(const std::string& player);

  // The party: the host starts one, invites friends, and everyone who accepts joins the same hidden match.
  bool CreateParty();
  bool InviteToParty(const std::string& player);
  bool AcceptPartyInvite(const std::string& invite);
  void DeclinePartyInvite(const std::string& invite);
  // The accepted invite once its party is joined; empty before, and after it was joined, declined or ended.
  std::optional<AcceptedInvite> AcceptedInviteInfo() const;
  // The game declined the accepted invite, or left it unjoined.
  void DropAcceptedInvite(const std::string& reason);
  void KickFromParty(const std::string& player);
  void LeaveParty();
  // The game left its session: a party serves one game session, so whoever was in it leaves it, and the next match gets
  // its own party, room and relay. An accepted invite the game has not joined yet keeps its party.
  void LeaveFinishedMatch();
  std::optional<Party> party() const;
  int my_slot() const;
  std::vector<Invite> Invites() const;

  // Advertises the party's game session to its members until cancelled; the party is made first when there is none.
  void HostPlayerMatch(const PlayerMatchRules& rules, int slots = kMaxParty);
  void CancelPlayerMatch();
  // The game's QoS listen data for its hosted session; members' QoS probes return it.
  void SetPlayerMatchQos(const std::vector<uint8_t>& data);
  // While on, the party's advertisement is fetched again every few seconds; PlayerMatchResults() holds the last answer.
  void SetPlayerMatchSearch(bool on);
  std::vector<PlayerMatchAd> PlayerMatchResults() const;
  // One guest search call. The service is asked again when its last answer is no longer fresh and no search is under
  // way; while `pending`, the answer for `epoch` is on its way and the guest must not be told "no session" yet.
  struct PlayerMatchSearchWait {
    uint32_t epoch = 0;
    bool pending = false;
  };
  PlayerMatchSearchWait AskPlayerMatchSearch();
  bool PlayerMatchSearchPending(uint32_t epoch) const;
  // Empty once `epoch` was left or replaced by a newer search.
  std::vector<PlayerMatchAd> PlayerMatchResults(uint32_t epoch) const;
  // False when the session is not among the last results or this player is not connected.
  bool JoinPlayerMatch(const std::string& session);
  // The join under way, then the service's refusal of the last one; `refused` stays empty for a join it accepted.
  struct PlayerMatchJoinAnswer {
    bool pending = false;
    std::string refused;
  };
  PlayerMatchJoinAnswer JoinAnswer() const;

  bool SignedIn() const;
  OnlineProfile Profile() const;
  ServiceCapabilities Capabilities() const;
  // The other players of the party that the game can talk to, with their slots.
  std::vector<ActivePeer> Peers() const;
  void TestConnection();
  // A DEBUG_SAULO line from the launcher page, in the same log.
  void LogLine(const std::string& line) {
    Log(line);
    FlushLog();
  }
  // From the game runtime, for one peer.
  void SetPeerLink(int slot, NetplayLink link);

  Snapshot snapshot() const;
  // The highest round trip among the connected peers (the worst connection of the party), without a full snapshot.
  std::optional<int> WorstRtt() const;
  std::vector<std::string> TakeNotices();
  std::vector<NetplayAssignment> Assignments() const;
  std::optional<NetplayAssignment> Assignment(int slot) const;
  // The game called XSessionStart: telemetry follows whether every other member then exchanges game traffic.
  void NoteGameStart();
  const std::string& url() const { return config_.url; }

 private:
  enum class Step { kIdle, kOpening, kGreeting, kOnline };
  enum class RoomStep { kNone, kOpening, kGreeting, kOnline };
  enum class Purpose {
    kHello, kFriends, kRequests, kAdd, kAccept, kDecline, kRemove, kPlayerMatchHost, kPlayerMatchCancel,
    kPlayerMatchSearch, kPlayerMatchJoin, kPartyCreate, kPartyLeave, kPartyKick, kInviteSend, kInviteAccept,
    kInviteDecline, kRoomHello, kTelemetry
  };
  struct Pending {
    Purpose purpose = Purpose::kHello;
    std::string player;
    uint32_t search_epoch = 0;
    double sent_at = 0;
  };

  void OpenChannel(double now);
  void PumpChannel(double now);
  void OnEvent(const ChannelEvent& event, double now);
  void OnText(const std::string& text, double now);
  void OnResponse(const ServerMessage& message, double now);
  void OnServerEvent(const ServerMessage& message, double now);
  void OnGreeted(const ServerMessage& message, double now);
  void OnLinkLost(const std::string& why, double now);
  void PumpPlayerMatch();
  void SendPlayerMatchHost();
  void SendPlayerMatchSearch();
  void BeginPlayerMatchSearch();
  void StopPlayerMatchSearch();
  bool PlayerMatchSearchInFlight(double* sent_at) const;
  void UpdateFriend(const Friend& entry);
  void Refresh();
  void Send(Purpose purpose, const std::function<std::string(int)>& encode, const std::string& player = {});
  void SendLocked(const std::string& text);
  bool Connected() const { return step_ == Step::kOnline; }
  void Notice(const std::string& text);
  void Log(const std::string& line);
  void FlushLog();
  void Disable();
  void StartTest();
  void CollectTest();
  bool EnsureIdentity();
  std::string SessionName() const;
  std::string NameOf(const std::string& player) const;

  // The party and its room.
  void AdoptAccess(const PartyAccess& access);
  void AdoptHostAccess(const PartyAccess& access);
  void OnParty(const Party& party);
  void EndParty(const std::string& reason);
  // The service ended the party; a game running over direct links outlives that.
  void ServiceEndedParty(const std::string& reason);
  bool PlayingDirect() const;
  void KeepPlayingWithoutService(const std::string& reason);
  void ExpireAcceptedInvite(double now);
  void OpenRoom(double now);
  void PumpRoom(double now);
  void OnRoomEvent(const ChannelEvent& event, double now);
  void OnRoomMessage(const ServerMessage& message, double now);
  void OnRoomHello(const ServerMessage& message, double now);
  void OnRoomLost(const std::string& why, double now);
  void CloseRoom();
  void StartSession(uint32_t relay_token, int generation);
  void SendRoom(const std::string& text);
  void FlushTelemetry(double now, bool force = false);
  void Emit(const SessionEvent& event);
  TelemetryContext MakeContext(const std::string& diag, int slot, int party_size) const;
  void RestoreInflightTelemetry();
  void UpdateGameStart(double now);
  void FinishGameStart(const std::string& reason);
  void SendTestBeacon(const ConnectionReport& report);
  void DropAcceptedInviteLocked(const std::string& reason);

  class Locked;

  ServiceConfig config_;
  mutable std::mutex mutex_;
  // Lines wait here while the service is locked, since the game's threads wait on that lock. Also written from the
  // gameplay socket's threads, which do not hold it.
  std::mutex log_mutex_;
  std::vector<std::string> log_queue_;
  std::optional<AcceptedInvite> announce_invite_;  // goes out once the service is unlocked
  UserSettings settings_;
  Identity identity_;
  bool identity_loaded_ = false;
  double next_identity_try_ = 0;

  Connection connection_ = Connection::kOff;
  std::string connection_text_;
  // Shared with the gameplay socket's relay, which sends on it from the game's threads.
  std::shared_ptr<LobbyChannel> channel_;
  std::shared_ptr<LobbyChannel> leaving_;
  ServiceCapabilities capabilities_;
  double leaving_deadline_ = 0;
  double now_ = 0;
  double step_deadline_ = 0;
  Step step_ = Step::kIdle;
  double retry_at_ = 0;
  double retry_delay_ = 1.0;
  double next_refresh_ = 0;
  double name_since_ = 0;
  std::string greeted_name_;
  int next_seq_ = 1;
  std::map<int, Pending> pending_;

  std::string friend_code_;
  bool fetched_ = false;
  std::vector<Friend> friends_;
  std::vector<Request> incoming_, outgoing_;
  std::optional<PlayerMatchRules> hosting_;
  int hosting_slots_ = kMaxParty;
  std::string hosting_session_;
  std::string hosting_qos_;
  double next_host_refresh_ = 0;
  bool searching_ = false;
  double next_search_ = 0;
  std::vector<PlayerMatchAd> results_;
  int search_generation_ = 0;
  uint32_t search_epoch_ = 0;
  double search_answered_at_ = -1e9;
  bool search_from_game_ = false;  // ends once the game stops asking
  double search_asked_at_ = 0;
  std::vector<Invite> invites_;
  std::optional<AcceptedInvite> accepted_invite_;
  uint32_t accepted_generation_ = 0;
  double accepted_deadline_ = 0;
  bool joining_ = false;
  bool abandon_join_ = false;  // the game left while its join was on its way
  std::string join_refused_;
  std::string notice_;
  double notice_at_ = 0;
  std::vector<std::string> notices_;

  std::optional<PartyAccess> access_;
  std::optional<Party> party_;
  uint32_t match_generation_ = 0;
  bool party_check_ = false;      // signed in again: a party this client holds no seat in was left behind on the service
  bool signaling_ended_ = false;  // the service ended the party; the game plays on until it leaves
  std::shared_ptr<LobbyChannel> room_;
  // The relay sends from the game's threads, and must follow a room that reconnects.
  struct RoomRoute {
    std::mutex mutex;
    std::weak_ptr<LobbyChannel> room;
  };
  std::shared_ptr<RoomRoute> room_route_ = std::make_shared<RoomRoute>();
  RoomStep room_step_ = RoomStep::kNone;
  double room_deadline_ = 0;
  double room_retry_at_ = 0;
  double room_retry_delay_ = 1.0;
  int room_generation_seen_ = 0;
  int room_hello_seq_ = 0;
  int room_hello_failures_ = 0;
  std::vector<RoomMember> room_members_;
  std::unique_ptr<MatchSession> session_;
  Telemetry telemetry_;
  TelemetryContext telemetry_context_;
  double next_telemetry_ = 0;
  std::string lobby_diag_;
  struct TelemetryInflight {
    int seq = 0;
    std::string batch;
    double sent_at = 0;
  } telemetry_inflight_;
  struct GameStart {
    bool active = false;
    double since = 0;
  } game_start_;
  double party_started_ = 0;

  struct TestState;
  std::shared_ptr<TestState> test_probe_;
  std::optional<ConnectionReport> test_;
  bool testing_ = false;
  bool test_needs_identity_ = false;
  int tests_finished_ = 0;
};

}  // namespace tmnt::online
