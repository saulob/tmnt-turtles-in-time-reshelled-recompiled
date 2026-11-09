#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "online/party_report.h"
#include "online/snes_features.h"

namespace tmnt::online {

inline constexpr int kLobbyProtocol = 4;
inline constexpr int kMaxParty = 4;
inline constexpr char kGameId[] = "tmnt-reshelled-pc";
// The one recompiled game image this port runs.
inline constexpr char kGuestBase[] = "base";
// Raised whenever a change stops this build from playing with the previous one.
inline constexpr int kOnlineBuild = 1;

enum class FriendState { kOffline, kOnline, kParty, kPlayerMatch, kConnecting, kInGame };

struct Friend {
  std::string player;  // the friend code, XXXX-XXXX
  std::string name;
  bool online = false;
  FriendState state = FriendState::kOffline;
  std::string platform;
  std::string version;
  std::string guest;
  bool compatible = true;
};

struct Request {
  std::string player;
  std::string name;
};

// A player who chose to be listed in the public Lobby. `player` is the friend code Add friend sends to; it is never shown.
struct LobbyPlayer {
  std::string player;
  std::string name;
  FriendState state = FriendState::kOnline;
  std::string platform;
  std::string version;
  std::string guest;
  bool compatible = true;
};

// The game's Player Match settings that both sides must agree on, as small named integers.
using PlayerMatchRules = std::vector<std::pair<std::string, int>>;

// A friend's advertised Player Match, as a search returns it.
struct PlayerMatchAd {
  std::string session;
  std::string player;
  std::string name;
  std::string guest;
  int build = 0;
  int slots = 2;
  PlayerMatchRules rules;
  std::string qos;  // the host's QoS listen data, lower-case hex
  std::string platform;
  std::string version;
};

// A friend's invitation to their party. Accepting it puts the player in the party's one hidden match.
struct Invite {
  std::string id;
  std::string from_player;
  std::string from_name;
  std::string session;  // the host's advertised game session; empty when the host has none
  int slots = kMaxParty;  // the player slots of that session
};

// The invite the player accepted, kept until the game joins it or it ends.
struct AcceptedInvite {
  uint32_t generation = 0;
  std::string session;
  std::string host;  // friend code
  std::string host_name;
  std::string invite;
  bool ready = false;  // the party is joined, so the game can be told
  int slots = kMaxParty;
};

enum class MemberState { kJoined, kConnecting, kConnected, kDisconnected };
const char* MemberStateName(MemberState state);

struct PartyMember {
  int slot = 0;  // 1 is the host
  std::string player;
  std::string name;
  std::string platform;
  std::string version;
  MemberState state = MemberState::kJoined;
};

struct PartyInvited {
  std::string player;
  std::string name;
};

// The user-facing party: the host plus up to three friends, over one hidden match on the service.
struct Party {
  std::string match;
  std::string diag;  // non-personal id every member and the service log, for correlating reports
  std::string host;  // the host's friend code
  int generation = 0;
  std::string session;  // the host's game-session advertisement, once the game has made one
  int slots = kMaxParty;  // the most players that session takes
  std::vector<PartyMember> members;
  std::vector<PartyInvited> invited;
  const PartyMember* member(int slot) const;
  const PartyMember* member_of(const std::string& player) const;
};

// What the service gives a player who creates or joins a party.
struct PartyAccess {
  std::string match;
  std::string diag;
  int slot = 0;
  std::string join_token;  // shown to its owner only; never logged
  int generation = 0;
  std::optional<Party> party;
};

// A participant as the match room reports it.
struct RoomLink {
  int peer = 0;
  std::string transport;  // pending, direct, relay
  std::string state;      // connecting, connected, lost
};
struct RoomMember {
  int slot = 0;
  std::string name;
  std::string state;  // joined, connecting, connected, disconnected
  std::vector<RoomLink> links;
};

// What the service says it can do, in its hello answer; an older service says nothing, which reads as all false.
struct ServiceCapabilities {
  bool announced = false;
  int protocol_version = 0;
  std::string worker_build;
  bool public_endpoints = false;
  bool relay_supported = false;
  int relay_max_payload = 0;
  bool snes_features = false;
  bool snes_modes = false;
  bool public_lobby = false;  // the service has the public Lobby (lobby.publish, lobby.list)
  bool bug_reports = false;   // the service takes problem reports (incident.file, incident.status, incident.read)
};

struct PeerInfo {
  int slot = 0;
  std::string name;
  std::string ip;
  uint16_t port = 0;
  std::vector<std::string> lan;
  bool same_network = false;
  std::string public_ip;  // its gameplay socket's STUN-mapped endpoint; empty when it sent none
  uint16_t public_port = 0;
  bool service_forwards_public = false;  // the service passes public endpoints on at all (an older one does not)
};

struct ServerMessage {
  enum class Kind {
    kResponse,
    kFriendRequest,
    kFriendAdded,
    kFriendRemoved,
    kFriendStatus,
    kFriendOffline,
    kMatchPeer,
    kMatchMembers,
    kMatchEnded,
    kMatchLeft,
    kMatchPeerLost,
    kMatchPeerBack,
    kInvite,
    kInviteEnded,
    kPartyUpdate,
    kPartyEnded,
    kMatchTransport,
    kRelayClosed,
    kMatchSnes,
  };
  Kind kind = Kind::kResponse;

  int seq = 0;
  std::string code;  // responses: "ok" or an error code
  std::string friend_code;
  std::optional<std::vector<Friend>> friends;
  std::optional<std::vector<Request>> incoming;
  std::optional<std::vector<Request>> outgoing;
  std::string state;  // friends.add: "pending" or "friends"
  std::string session;  // pm.host
  std::optional<std::vector<PlayerMatchAd>> sessions;  // pm.search
  std::optional<PartyAccess> access;     // party.create, invite.accept, pm.host that made the party, pm.join
  std::optional<Party> party;            // party.update
  std::optional<std::vector<RoomMember>> room_members;  // match.members, and the match.hello answer
  uint32_t relay_token = 0;              // match.hello answer
  int generation = 0;                    // match.hello answer, match.members
  int slot = 0;                          // match.left, match.peer_lost, match.peer_back, relay.closed
  int pair_a = 0, pair_b = 0, pair_by = 0;  // match.transport

  Friend friend_view;  // friend.* events
  Request request;     // friend.request
  std::string player;  // friend.removed, friend.offline
  std::string reason;  // match.ended, invite.ended
  std::optional<Invite> invite;  // invite
  std::string invite_id;         // invite.send, invite.ended
  std::string match_id;
  PeerInfo peer;
  std::string transport;  // match.transport: "direct" or "relay"
  std::optional<ServiceCapabilities> capabilities;  // hello
  std::optional<MatchSnes> snes;  // match.hello and snes.update answers, match.snes
  std::optional<std::vector<LobbyPlayer>> lobby_players;  // lobby.list; absent when unchanged
  bool lobby_unchanged = false;                           // lobby.list: the revision the client sent is still current
  int64_t lobby_rev = -1;                                 // lobby.list: -1 when the answer carried none
  std::optional<bool> lobby_visible;                      // lobby.publish and lobby.list: whether this connection is listed
};

// Nothing for text that is not a known, well-formed server message.
std::optional<ServerMessage> DecodeServerMessage(std::string_view text);

struct HelloFields {
  std::string guest;
  int build = 0;
  std::string player;
  std::string token;
  std::string name;
  std::string version;
  std::string platform;
  bool presence = true;  // false: a short session that is not shown to friends (the connection test)
  bool relay = false;    // this client can play a match through the service's relay
};

std::string EncodeHello(int seq, const HelloFields& hello);
std::string EncodePing(int seq);
std::string EncodeLobbyPublish(int seq, bool visible);
// `held_rev` is the revision of the list the client already shows; the service then answers "unchanged" while it stands.
std::string EncodeLobbyList(int seq, std::optional<int64_t> held_rev = std::nullopt);
std::string EncodeFriendsList(int seq);
std::string EncodeFriendsRequests(int seq);
std::string EncodeFriendsAdd(int seq, const std::string& code);
std::string EncodeFriendsAccept(int seq, const std::string& player);
std::string EncodeFriendsDecline(int seq, const std::string& player);
std::string EncodeFriendsRemove(int seq, const std::string& player);
std::string EncodePlayerMatchHost(int seq, const PlayerMatchRules& rules, const std::string& qos_hex = {}, int slots = 4);
std::string EncodePlayerMatchCancel(int seq);
std::string EncodePlayerMatchSearch(int seq);
std::string EncodePlayerMatchJoin(int seq, const std::string& session);
std::string EncodePartyCreate(int seq);
std::string EncodePartyLeave(int seq);
std::string EncodePartyKick(int seq, const std::string& player);
std::string EncodePartyReport(int seq, const PartySnapshot& snapshot);
std::string EncodeInviteSend(int seq, const std::string& player);
std::string EncodeInviteAccept(int seq, const std::string& invite);
std::string EncodeInviteDecline(int seq, const std::string& invite);
std::string EncodeMatchHello(int seq, const std::string& match, int slot, const std::string& join_token,
                             int generation_seen, const SnesGameplayCapabilities& snes = {});
std::string EncodeSnesUpdate(int seq, const SnesGameplayCapabilities& snes);
// The game started its `round`th match of the party: the room fixes that match's table for everyone. `off` asks for all
// Off when nothing is fixed for the round yet.
std::string EncodeSnesLock(int seq, int round, bool off = false);
// `port` is the gameplay socket's local port; `public_ip`:`public_port` is that socket's STUN-mapped endpoint, left out
// when `public_port` is 0.
std::string EncodeMatchEndpoint(int seq, uint16_t port, const std::vector<std::string>& lan,
                                const std::string& public_ip = {}, uint16_t public_port = 0);
std::string EncodeMatchLeave(int seq);
// The lower slot of a pair chooses its transport, "direct" or "relay"; the room tells the other end and the host.
std::string EncodeMatchTransport(int seq, int peer_slot, const std::string& transport);
std::string EncodeMatchLink(int seq, int peer_slot, const std::string& state);

std::string HexOf(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> BytesOfHex(std::string_view hex);

// "abcd 2345" and "ABCD-2345" both give "ABCD-2345"; empty when the text is not a friend code.
std::string NormalizeFriendCode(std::string_view text);

// User-facing sentence for an ended invite; empty when the ending needs no explanation.
std::string DescribeInviteEnded(const std::string& reason, bool received);
// User-facing sentence for a service error code.
std::string DescribeServiceError(const std::string& code);
// The same for a join or an accept, where "not found" is a match rather than a friend code.
std::string DescribeJoinError(const std::string& code);

// The same rule the game sessions need: same recompiled image and same netplay build.
bool GuestsCompatible(const std::string& guest_a, int build_a, const std::string& guest_b, int build_b);

// The address the joiner uses for the host's gameplay port. `mine` are this PC's own LAN candidates.
std::string ChoosePeerAddress(const std::vector<std::string>& mine, const PeerInfo& peer);

// "Online · Windows · v0.2.2" (or "Online Match" / "In game" first) for a friend that is online, "Offline" otherwise.
std::string FriendStatusLine(const Friend& entry);

}  // namespace tmnt::online
