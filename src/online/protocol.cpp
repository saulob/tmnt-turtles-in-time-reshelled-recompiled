#include "online/protocol.h"

#include <algorithm>
#include <cctype>

#include "updater/json.h"

namespace tmnt::online {
namespace json = trg::update::json;
namespace {

constexpr char kLoopback[] = "127.0.0.1";

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0x0F];
          out += kHex[c & 0x0F];
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string Envelope(const char* op, int seq, const std::string& fields = {}) {
  return std::string("{\"op\":") + Quote(op) + ",\"seq\":" + std::to_string(seq) + fields + "}";
}

std::string WithPlayer(const char* op, int seq, const std::string& player) {
  return Envelope(op, seq, ",\"player\":" + Quote(player));
}

std::string StringOf(const json::Value* v) { return v && v->IsString() ? v->AsString() : std::string(); }
bool BoolOf(const json::Value* v, bool fallback) { return v && v->IsBool() ? v->AsBool() : fallback; }
int IntOf(const json::Value* v) {
  const auto n = v && v->IsNumber() ? v->AsInt64() : std::nullopt;
  return n ? int(*n) : 0;
}

FriendState StateOf(const std::string& text) {
  if (text == "online") return FriendState::kOnline;
  if (text == "party") return FriendState::kParty;
  if (text == "player_match") return FriendState::kPlayerMatch;
  if (text == "connecting") return FriendState::kConnecting;
  if (text == "in_game") return FriendState::kInGame;
  return FriendState::kOffline;
}

std::optional<Friend> ReadFriend(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  Friend f;
  f.player = StringOf(v->Find("player"));
  f.name = StringOf(v->Find("name"));
  if (f.player.empty()) return std::nullopt;
  f.online = BoolOf(v->Find("online"), false);
  f.state = f.online ? StateOf(StringOf(v->Find("state"))) : FriendState::kOffline;
  f.platform = StringOf(v->Find("platform"));
  f.version = StringOf(v->Find("version"));
  f.guest = StringOf(v->Find("guest"));
  f.compatible = BoolOf(v->Find("compatible"), true);
  return f;
}

std::optional<std::vector<Friend>> ReadFriends(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<Friend> out;
  for (const json::Value& row : v->AsArray()) {
    const std::optional<Friend> f = ReadFriend(&row);
    if (!f) return std::nullopt;
    out.push_back(*f);
  }
  return out;
}

std::optional<std::vector<Request>> ReadRequests(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<Request> out;
  for (const json::Value& row : v->AsArray()) {
    if (!row.IsObject()) return std::nullopt;
    Request r{StringOf(row.Find("player")), StringOf(row.Find("name"))};
    if (r.player.empty()) return std::nullopt;
    out.push_back(std::move(r));
  }
  return out;
}

std::optional<PlayerMatchAd> ReadPlayerMatch(const json::Value& row) {
  if (!row.IsObject()) return std::nullopt;
  PlayerMatchAd ad;
  ad.session = StringOf(row.Find("session"));
  ad.player = StringOf(row.Find("player"));
  ad.name = StringOf(row.Find("name"));
  ad.guest = StringOf(row.Find("guest"));
  ad.build = IntOf(row.Find("build"));
  ad.slots = IntOf(row.Find("slots"));
  ad.platform = StringOf(row.Find("platform"));
  ad.qos = StringOf(row.Find("qos"));
  ad.version = StringOf(row.Find("version"));
  if (const json::Value* rules = row.Find("rules"); rules && rules->IsObject()) {
    for (const auto& [key, value] : rules->AsObject()) {
      if (value.IsNumber()) ad.rules.emplace_back(key, IntOf(&value));
    }
  }
  if (ad.session.empty() || ad.player.empty() || ad.guest.empty()) return std::nullopt;
  return ad;
}

std::optional<std::vector<PlayerMatchAd>> ReadPlayerMatches(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<PlayerMatchAd> out;
  for (const json::Value& row : v->AsArray()) {
    std::optional<PlayerMatchAd> ad = ReadPlayerMatch(row);
    if (!ad) return std::nullopt;
    out.push_back(std::move(*ad));
  }
  return out;
}

uint32_t RelayTokenOf(const json::Value* v) {
  const auto n = v && v->IsNumber() ? v->AsInt64() : std::nullopt;
  return n && *n > 0 && *n <= 0xFFFFFFFFll ? uint32_t(*n) : 0;
}

ServiceCapabilities ReadCapabilities(const json::Value& v) {
  ServiceCapabilities caps;
  caps.announced = true;
  caps.protocol_version = IntOf(v.Find("protocol_version"));
  caps.worker_build = StringOf(v.Find("worker_build"));
  caps.public_endpoints = BoolOf(v.Find("public_endpoints"), false);
  caps.relay_supported = BoolOf(v.Find("relay_supported"), false);
  caps.relay_max_payload = IntOf(v.Find("relay_max_payload"));
  caps.snes_features = BoolOf(v.Find("snes_features"), false);
  return caps;
}

SnesGameplaySet ReadSnesSet(const json::Value* v) {
  SnesGameplaySet set;
  if (!v || !v->IsObject()) return set;
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const SnesFeature feature = SnesFeatureAt(i);
    set.Set(feature, BoolOf(v->Find(SnesKey(feature)), false));
  }
  return set;
}

uint32_t ParseCompat(const std::string& text) {
  if (text == "mixed") return kSnesCompatMixed;
  if (text.empty() || text.size() > 8) return 0;
  uint32_t value = 0;
  for (const char c : text) {
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (digit < 0) return 0;
    value = value << 4 | uint32_t(digit);
  }
  return value == kSnesCompatMixed ? 0 : value;
}

SnesGameplayCapabilities ReadSnesCapabilities(const json::Value* v) {
  SnesGameplayCapabilities caps;
  if (!v || !v->IsObject()) return caps;
  caps.supports = ReadSnesSet(v->Find("supports"));
  caps.enabled = ReadSnesSet(v->Find("enabled"));
  if (const json::Value* compat = v->Find("compat"); compat && compat->IsObject()) {
    for (int i = 0; i < kSnesFeatureCount; ++i) {
      caps.compat[size_t(i)] = ParseCompat(StringOf(compat->Find(SnesKey(SnesFeatureAt(i)))));
    }
  }
  return caps.Normalized();
}

// `players` is keyed by party slot; a seat outside 1 to 4 or a row that is not an object is left out.
SnesMatchTable ReadSnesTable(const json::Value& v) {
  SnesMatchTable table;
  table.supported = ReadSnesSet(v.Find("supported"));
  if (const json::Value* players = v.Find("players"); players && players->IsObject()) {
    for (int slot = 1; slot <= kSnesSeats; ++slot) {
      const json::Value* row = players->Find(std::to_string(slot));
      if (row && row->IsObject()) table.players[size_t(slot - 1)] = ReadSnesSet(row);
    }
  }
  return table;
}

std::optional<MatchSnes> ReadMatchSnes(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  MatchSnes snes;
  if (const json::Value* peer = v->Find("peer"); peer && peer->IsObject()) snes.peer = ReadSnesCapabilities(peer);
  snes.table = ReadSnesTable(*v);
  if (const json::Value* locked = v->Find("locked"); locked && locked->IsObject()) {
    const int round = IntOf(locked->Find("round"));
    if (round > 0) snes.locked = SnesMatchLock{round, ReadSnesTable(*locked)};
  }
  return snes;
}

std::string SnesSetJson(const SnesGameplaySet& set) {
  std::string out = "{";
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const SnesFeature feature = SnesFeatureAt(i);
    out += std::string(i ? "," : "") + "\"" + SnesKey(feature) + "\":" + (set.Has(feature) ? "true" : "false");
  }
  return out + "}";
}

std::string SnesJson(const SnesGameplayCapabilities& snes) {
  const SnesGameplayCapabilities caps = snes.Normalized();
  static constexpr char kHex[] = "0123456789abcdef";
  std::string compat = "{";
  bool first = true;
  for (int i = 0; i < kSnesFeatureCount; ++i) {
    const uint32_t id = caps.compat[size_t(i)];
    if (!id || id == kSnesCompatMixed) continue;
    std::string hex(8, '0');
    for (int digit = 0; digit < 8; ++digit) hex[size_t(7 - digit)] = kHex[(id >> (digit * 4)) & 0xF];
    compat += std::string(first ? "" : ",") + "\"" + SnesKey(SnesFeatureAt(i)) + "\":\"" + hex + "\"";
    first = false;
  }
  return "{\"supports\":" + SnesSetJson(caps.supports) + ",\"enabled\":" + SnesSetJson(caps.enabled) +
         ",\"compat\":" + compat + "}}";
}

MemberState MemberStateOf(const std::string& text) {
  if (text == "connecting") return MemberState::kConnecting;
  if (text == "connected") return MemberState::kConnected;
  if (text == "disconnected") return MemberState::kDisconnected;
  return MemberState::kJoined;
}

// A service that does not say offers the whole party.
int SlotsOf(const json::Value* v) {
  const int slots = IntOf(v);
  return slots >= 2 && slots <= kMaxParty ? slots : kMaxParty;
}

std::optional<Party> ReadParty(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  Party party;
  party.match = StringOf(v->Find("match"));
  party.diag = StringOf(v->Find("diag"));
  party.host = StringOf(v->Find("host"));
  party.generation = IntOf(v->Find("generation"));
  party.session = StringOf(v->Find("session"));
  party.slots = SlotsOf(v->Find("slots"));
  if (party.match.empty()) return std::nullopt;
  if (const json::Value* members = v->Find("members"); members && members->IsArray()) {
    for (const json::Value& row : members->AsArray()) {
      PartyMember member;
      member.slot = IntOf(row.Find("slot"));
      member.player = StringOf(row.Find("player"));
      member.name = StringOf(row.Find("name"));
      member.platform = StringOf(row.Find("platform"));
      member.version = StringOf(row.Find("version"));
      member.state = MemberStateOf(StringOf(row.Find("state")));
      if (member.slot >= 1 && member.slot <= kMaxParty && !member.player.empty()) party.members.push_back(member);
    }
  }
  if (const json::Value* invited = v->Find("invited"); invited && invited->IsArray()) {
    for (const json::Value& row : invited->AsArray()) {
      const std::string player = StringOf(row.Find("player"));
      if (!player.empty()) party.invited.push_back({player, StringOf(row.Find("name"))});
    }
  }
  return party;
}

std::optional<std::vector<RoomMember>> ReadRoomMembers(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<RoomMember> members;
  for (const json::Value& row : v->AsArray()) {
    RoomMember member;
    member.slot = IntOf(row.Find("slot"));
    member.name = StringOf(row.Find("name"));
    member.state = StringOf(row.Find("state"));
    if (const json::Value* links = row.Find("links"); links && links->IsArray()) {
      for (const json::Value& link : links->AsArray()) {
        member.links.push_back({IntOf(link.Find("peer")), StringOf(link.Find("transport")), StringOf(link.Find("state"))});
      }
    }
    if (member.slot >= 1 && member.slot <= kMaxParty) members.push_back(member);
  }
  return members;
}

std::optional<PartyAccess> ReadAccess(const json::Value& v) {
  PartyAccess access;
  access.match = StringOf(v.Find("match"));
  access.diag = StringOf(v.Find("diag"));
  access.slot = IntOf(v.Find("slot"));
  access.join_token = StringOf(v.Find("join_token"));
  access.generation = IntOf(v.Find("generation"));
  access.party = ReadParty(v.Find("party"));
  if (access.match.empty() || access.slot < 1 || access.slot > kMaxParty) return std::nullopt;
  return access;
}

bool IsLoopback(const std::string& ip) { return ip.rfind("127.", 0) == 0; }

}  // namespace

std::optional<ServerMessage> DecodeServerMessage(std::string_view text) {
  const std::optional<json::Value> root = json::Parse(text);
  if (!root || !root->IsObject()) return std::nullopt;
  ServerMessage message;
  if (root->Find("re")) {
    message.kind = ServerMessage::Kind::kResponse;
    message.seq = IntOf(root->Find("re"));
    message.code = StringOf(root->Find("code"));
    if (message.code.empty()) return std::nullopt;
    message.friend_code = StringOf(root->Find("friend_code"));
    message.invite_id = StringOf(root->Find("invite"));
    message.state = StringOf(root->Find("state"));
    message.relay_token = RelayTokenOf(root->Find("relay_token"));
    message.generation = IntOf(root->Find("generation"));
    message.room_members = ReadRoomMembers(root->Find("members"));
    message.party = ReadParty(root->Find("party"));
    message.sessions = ReadPlayerMatches(root->Find("sessions"));
    message.session = StringOf(root->Find("session"));
    message.friends = ReadFriends(root->Find("friends"));
    message.incoming = ReadRequests(root->Find("incoming"));
    message.outgoing = ReadRequests(root->Find("outgoing"));
    if (const json::Value* caps = root->Find("capabilities"); caps && caps->IsObject()) {
      message.capabilities = ReadCapabilities(*caps);
    }
    if (root->Find("match") && root->Find("slot")) message.access = ReadAccess(*root);
    message.snes = ReadMatchSnes(root->Find("snes"));
    return message;
  }
  const std::string event = StringOf(root->Find("ev"));
  using Kind = ServerMessage::Kind;
  if (event == "friend.status" || event == "friend.added") {
    const std::optional<Friend> f = ReadFriend(root->Find("friend"));
    if (!f) return std::nullopt;
    message.kind = event == "friend.status" ? Kind::kFriendStatus : Kind::kFriendAdded;
    message.friend_view = *f;
    return message;
  }
  if (event == "friend.request") {
    message.kind = Kind::kFriendRequest;
    message.request = {StringOf(root->Find("player")), StringOf(root->Find("name"))};
    return message.request.player.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "friend.removed" || event == "friend.offline") {
    message.kind = event == "friend.removed" ? Kind::kFriendRemoved : Kind::kFriendOffline;
    message.player = StringOf(root->Find("player"));
    return message.player.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "invite") {
    const json::Value* invite = root->Find("invite");
    const json::Value* from = root->Find("from");
    const std::string id = invite ? (invite->IsString() ? invite->AsString() : StringOf(invite->Find("id"))) : std::string();
    if (id.empty() || !from || !from->IsObject()) return std::nullopt;
    message.kind = Kind::kInvite;
    message.invite = Invite{id, StringOf(from->Find("player")), StringOf(from->Find("name")), StringOf(root->Find("session"))};
    message.invite->slots = SlotsOf(root->Find("slots"));
    return message.invite->from_player.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "invite.ended") {
    message.kind = Kind::kInviteEnded;
    message.invite_id = StringOf(root->Find("invite"));
    message.reason = StringOf(root->Find("reason"));
    return message.invite_id.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "party.update") {
    message.kind = Kind::kPartyUpdate;
    message.party = ReadParty(root->Find("party") ? root->Find("party") : &*root);
    return message.party ? std::optional<ServerMessage>(message) : std::nullopt;
  }
  if (event == "party.ended") {
    message.kind = Kind::kPartyEnded;
    message.match_id = StringOf(root->Find("match"));
    message.reason = StringOf(root->Find("reason"));
    return message.match_id.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "match.members") {
    message.kind = Kind::kMatchMembers;
    message.room_members = ReadRoomMembers(root->Find("members"));
    message.generation = IntOf(root->Find("generation"));
    return message.room_members ? std::optional<ServerMessage>(message) : std::nullopt;
  }
  if (event == "match.snes") {
    message.kind = Kind::kMatchSnes;
    message.snes = ReadMatchSnes(root->Find("snes"));
    return message.snes ? std::optional<ServerMessage>(message) : std::nullopt;
  }
  if (event == "match.ended") {
    message.kind = Kind::kMatchEnded;
    message.reason = StringOf(root->Find("reason"));
    return message;
  }
  if (event == "match.left" || event == "match.peer_lost" || event == "match.peer_back" || event == "relay.closed") {
    message.kind = event == "match.left" ? Kind::kMatchLeft
                   : event == "match.peer_lost" ? Kind::kMatchPeerLost
                   : event == "match.peer_back" ? Kind::kMatchPeerBack : Kind::kRelayClosed;
    message.slot = IntOf(root->Find(event == "relay.closed" ? "peer" : "slot"));
    message.reason = StringOf(root->Find("reason"));
    const bool needs_slot = message.kind != Kind::kRelayClosed;
    return needs_slot && (message.slot < 1 || message.slot > kMaxParty) ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "match.transport") {
    message.kind = Kind::kMatchTransport;
    message.pair_a = IntOf(root->Find("a"));
    message.pair_b = IntOf(root->Find("b"));
    message.pair_by = IntOf(root->Find("by"));
    message.transport = StringOf(root->Find("transport"));
    const bool slots = message.pair_a >= 1 && message.pair_b >= 1 && message.pair_a <= kMaxParty && message.pair_b <= kMaxParty;
    const bool known = message.transport == "direct" || message.transport == "relay";
    return slots && known ? std::optional<ServerMessage>(message) : std::nullopt;
  }
  if (event == "match.peer") {
    message.kind = Kind::kMatchPeer;
    PeerInfo& peer = message.peer;
    peer.slot = IntOf(root->Find("slot"));
    peer.name = StringOf(root->Find("name"));
    peer.ip = StringOf(root->Find("ip"));
    const int port = IntOf(root->Find("port"));
    if (peer.slot < 1 || peer.slot > kMaxParty || port < 1 || port > 65535 || peer.ip.empty()) return std::nullopt;
    peer.port = uint16_t(port);
    peer.same_network = BoolOf(root->Find("same_network"), false);
    if (const json::Value* lan = root->Find("lan"); lan && lan->IsArray()) {
      for (const json::Value& ip : lan->AsArray()) {
        if (ip.IsString()) peer.lan.push_back(ip.AsString());
      }
    }
    peer.service_forwards_public = BoolOf(root->Find("service_forwards_public"), false) ||
                                   BoolOf(root->Find("public_endpoints"), false);
    const std::string public_ip = StringOf(root->Find("public_ip"));
    const int public_port = IntOf(root->Find("public_port"));
    if (!public_ip.empty() && public_port >= 1 && public_port <= 65535) {
      peer.public_ip = public_ip;
      peer.public_port = uint16_t(public_port);
    }
    return message;
  }
  return std::nullopt;
}

std::string EncodeHello(int seq, const HelloFields& hello) {
  return Envelope("hello", seq,
                 ",\"proto\":" + std::to_string(kLobbyProtocol) + ",\"game\":" + Quote(kGameId) +
                     ",\"guest\":" + Quote(hello.guest) + ",\"build\":" + std::to_string(hello.build) +
                     ",\"player\":" + Quote(hello.player) + ",\"token\":" + Quote(hello.token) +
                     ",\"name\":" + Quote(hello.name) + ",\"version\":" + Quote(hello.version) +
                     ",\"platform\":" + Quote(hello.platform) + ",\"presence\":" + (hello.presence ? "true" : "false") +
                     (hello.relay ? ",\"relay\":true" : ""));
}

std::string EncodePing(int seq) { return Envelope("ping", seq); }
std::string EncodePartyReport(int seq, const PartySnapshot& snapshot) {
  std::string characters;
  for (int i = 0; i < snapshot.size; ++i) characters += (i ? "," : "") + std::to_string(snapshot.characters[size_t(i)]);
  return Envelope("party.report", seq, ",\"characters\":[" + characters + "],\"host\":" + std::to_string(snapshot.host));
}
std::string EncodeFriendsList(int seq) { return Envelope("friends.list", seq); }
std::string EncodeFriendsRequests(int seq) { return Envelope("friends.requests", seq); }
std::string EncodeFriendsAdd(int seq, const std::string& code) { return Envelope("friends.add", seq, ",\"code\":" + Quote(code)); }
std::string EncodeFriendsAccept(int seq, const std::string& player) { return WithPlayer("friends.accept", seq, player); }
std::string EncodeFriendsDecline(int seq, const std::string& player) { return WithPlayer("friends.decline", seq, player); }
std::string EncodeFriendsRemove(int seq, const std::string& player) { return WithPlayer("friends.remove", seq, player); }
std::string EncodePlayerMatchHost(int seq, const PlayerMatchRules& rules, const std::string& qos_hex, int slots) {
  std::string object = "{";
  for (size_t i = 0; i < rules.size(); ++i) {
    object += (i ? "," : "") + Quote(rules[i].first) + ":" + std::to_string(rules[i].second);
  }
  const std::string qos = qos_hex.empty() ? std::string() : ",\"qos\":" + Quote(qos_hex);
  return Envelope("pm.host", seq, ",\"slots\":" + std::to_string(slots) + ",\"rules\":" + object + "}" + qos);
}

std::string EncodePlayerMatchCancel(int seq) { return Envelope("pm.cancel", seq); }
std::string EncodePlayerMatchSearch(int seq) { return Envelope("pm.search", seq); }
std::string EncodePlayerMatchJoin(int seq, const std::string& session) {
  return Envelope("pm.join", seq, ",\"session\":" + Quote(session));
}

std::string EncodePartyCreate(int seq) { return Envelope("party.create", seq); }
std::string EncodePartyLeave(int seq) { return Envelope("party.leave", seq); }
std::string EncodePartyKick(int seq, const std::string& player) { return WithPlayer("party.kick", seq, player); }
std::string EncodeInviteSend(int seq, const std::string& player) { return WithPlayer("invite.send", seq, player); }
std::string EncodeInviteAccept(int seq, const std::string& invite) {
  return Envelope("invite.accept", seq, ",\"invite\":" + Quote(invite));
}
std::string EncodeInviteDecline(int seq, const std::string& invite) {
  return Envelope("invite.decline", seq, ",\"invite\":" + Quote(invite));
}

std::string EncodeMatchHello(int seq, const std::string& match, int slot, const std::string& join_token,
                             int generation_seen, const SnesGameplayCapabilities& snes) {
  return Envelope("match.hello", seq,
                  ",\"proto\":" + std::to_string(kLobbyProtocol) + ",\"match\":" + Quote(match) +
                      ",\"slot\":" + std::to_string(slot) + ",\"token\":" + Quote(join_token) +
                      ",\"relay\":true,\"generation_seen\":" + std::to_string(generation_seen) +
                      ",\"snes\":" + SnesJson(snes));
}

std::string EncodeSnesLock(int seq, int round, bool off) {
  return Envelope("snes.lock", seq, ",\"round\":" + std::to_string(round) + (off ? ",\"off\":true" : ""));
}

std::string EncodeSnesUpdate(int seq, const SnesGameplayCapabilities& snes) {
  return Envelope("snes.update", seq, ",\"snes\":" + SnesJson(snes));
}

std::string EncodeMatchEndpoint(int seq, uint16_t port, const std::vector<std::string>& lan,
                                const std::string& public_ip, uint16_t public_port) {
  std::string list = "[";
  for (size_t i = 0; i < lan.size(); ++i) list += (i ? "," : "") + Quote(lan[i]);
  std::string mapped;
  if (public_port && !public_ip.empty()) {
    mapped = ",\"public_ip\":" + Quote(public_ip) + ",\"public_port\":" + std::to_string(public_port);
  }
  return Envelope("match.endpoint", seq, ",\"port\":" + std::to_string(port) + ",\"lan\":" + list + "]" + mapped);
}

std::string EncodeMatchLeave(int seq) { return Envelope("match.leave", seq); }
std::string EncodeMatchTransport(int seq, int peer_slot, const std::string& transport) {
  return Envelope("match.transport", seq, ",\"peer\":" + std::to_string(peer_slot) + ",\"transport\":" + Quote(transport));
}
std::string EncodeMatchLink(int seq, int peer_slot, const std::string& state) {
  return Envelope("match.link", seq, ",\"peer\":" + std::to_string(peer_slot) + ",\"state\":" + Quote(state));
}

const char* MemberStateName(MemberState state) {
  switch (state) {
    case MemberState::kJoined: return "joined";
    case MemberState::kConnecting: return "connecting";
    case MemberState::kConnected: return "connected";
    case MemberState::kDisconnected: return "disconnected";
  }
  return "joined";
}

const PartyMember* Party::member(int slot) const {
  for (const PartyMember& m : members) {
    if (m.slot == slot) return &m;
  }
  return nullptr;
}

const PartyMember* Party::member_of(const std::string& player) const {
  for (const PartyMember& m : members) {
    if (m.player == player) return &m;
  }
  return nullptr;
}

std::string HexOf(const std::vector<uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (const uint8_t b : bytes) {
    text += kHex[b >> 4];
    text += kHex[b & 0xF];
  }
  return text;
}

std::vector<uint8_t> BytesOfHex(std::string_view hex) {
  const auto digit = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::vector<uint8_t> bytes;
  if (hex.size() % 2) return bytes;
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = digit(hex[i]), lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    bytes.push_back(uint8_t(hi << 4 | lo));
  }
  return bytes;
}

std::string NormalizeFriendCode(std::string_view text) {
  std::string compact;
  for (const char c : text) {
    if (c == '-' || std::isspace(static_cast<unsigned char>(c))) continue;
    compact += char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (compact.size() != 8) return {};
  for (const char c : compact) {
    const bool letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'O';
    const bool digit = c >= '2' && c <= '9';
    if (!letter && !digit) return {};
  }
  return compact.substr(0, 4) + "-" + compact.substr(4);
}

std::string DescribeInviteEnded(const std::string& reason, bool received) {
  if (received) {
    if (reason == "expired") return "The invite expired.";
    if (reason == "host_left") return "The host has already left.";
    if (reason == "party_full") return "The party is full.";
    if (reason == "cancelled") return "The invite was cancelled.";
    return {};
  }
  if (reason == "declined") return "A friend declined your invite.";
  if (reason == "expired") return "Your invite expired.";
  return {};
}

std::string DescribeJoinError(const std::string& code) {
  if (code == "not_found" || code == "bad_request") return "That match is no longer available.";
  return DescribeServiceError(code);
}

std::string DescribeServiceError(const std::string& code) {
  if (code == "version_mismatch") return "Game versions do not match.";
  if (code == "not_found") return "No player has that friend code.";
  if (code == "not_friends") return "That player is not your friend.";
  if (code == "offline") return "Your friend is not online.";
  if (code == "busy") return "One of you is already in a match.";
  if (code == "bad_request") return "That is not a valid friend code.";
  if (code == "rate_limited" || code == "limit") return "Too many requests. Try again in a moment.";
  if (code == "proto") return "The online service needs a different version of the game.";
  if (code == "unauthorized" || code == "bad_game" || code == "bad_guest") return "The online service did not accept this game.";
  if (code == "unreachable" || code == "link_lost" || code == "timeout") return "Unable to reach online service.";
  if (code == "relay_unavailable") return "The connection through the online service is not available.";
  if (code == "party_full") return "The party is full.";
  if (code == "no_party") return "Start a party first.";
  if (code == "already_invited") return "That friend already has an invite.";
  if (code == "expired") return "The invite has expired.";
  if (code == "host_left") return "The host has already left.";
  return "The online service refused the request (" + code + ").";
}

bool GuestsCompatible(const std::string& guest_a, int build_a, const std::string& guest_b, int build_b) {
  return guest_a == guest_b && build_a == build_b;
}

std::string ChoosePeerAddress(const std::vector<std::string>& mine, const PeerInfo& peer) {
  if (!peer.same_network) return peer.ip;
  std::vector<std::string> remote_private;
  for (const std::string& ip : peer.lan) {
    if (!IsLoopback(ip)) remote_private.push_back(ip);
  }
  for (const std::string& ip : remote_private) {
    if (std::find(mine.begin(), mine.end(), ip) != mine.end()) return kLoopback;
  }
  return remote_private.empty() ? kLoopback : remote_private.front();
}

std::string FriendStatusLine(const Friend& entry) {
  if (!entry.online) return "Offline";
  std::string line;
  switch (entry.state) {
    case FriendState::kParty: line = "In a party"; break;
    case FriendState::kPlayerMatch: line = "Online Match"; break;
    case FriendState::kConnecting: line = "Connecting"; break;
    case FriendState::kInGame: line = "In game"; break;
    default: line = "Online"; break;
  }
  if (!entry.platform.empty()) line += " \xC2\xB7 " + entry.platform;
  if (!entry.version.empty()) line += " \xC2\xB7 v" + entry.version;
  return line;
}

}  // namespace tmnt::online
