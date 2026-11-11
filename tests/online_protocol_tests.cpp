// Online protocol encoding and decoding: requests, answers, events, malformed input, friend codes, version
// compatibility and the peer address choice. Headless.
// Run: tmnt_online_protocol_tests

#include <cstdio>
#include <string>

#include "online/protocol.h"

using namespace tmnt::online;

// A second game image, so the client keeps telling images apart even though this port ships one.
constexpr char kGuestOther[] = "other";

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

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestRequestsAreWellFormed() {
  const std::string hello = EncodeHello(3, {"other", 1, "ABCD-2345", std::string(64, 'a'), "Sa\"ulo", "0.2.2", "Windows", true});
  CHECK(hello.rfind("{\"op\":\"hello\",\"seq\":3,", 0) == 0);
  CHECK(Contains(hello, "\"proto\":4") && Contains(hello, "\"game\":\"tmnt-reshelled-pc\"") &&
        Contains(hello, "\"guest\":\"other\"") && Contains(hello, "\"build\":1") && Contains(hello, "\"name\":\"Sa\\\"ulo\"") &&
        Contains(hello, "\"presence\":true") && Contains(hello, "\"version\":\"0.2.2\"") &&
        Contains(hello, "\"platform\":\"Windows\""));
  CHECK(Contains(EncodeHello(1, {"base", 1, "p", "t", "n", "", "", false}), "\"presence\":false"));
  CHECK(EncodeFriendsAdd(4, "ABCD-2345") == "{\"op\":\"friends.add\",\"seq\":4,\"code\":\"ABCD-2345\"}");
  CHECK(EncodeFriendsAddPlayer(4, "0123456789abcdef0123456789abcdef") ==
        "{\"op\":\"friends.add\",\"seq\":4,\"player\":\"0123456789abcdef0123456789abcdef\"}");
  CHECK(EncodePlayerMatchHost(5, {{"rounds", 2}, {"timer", 1}}, {}, 2) ==
        "{\"op\":\"pm.host\",\"seq\":5,\"slots\":2,\"rules\":{\"rounds\":2,\"timer\":1}}");
  CHECK(EncodePlayerMatchHost(6, {}) == "{\"op\":\"pm.host\",\"seq\":6,\"slots\":4,\"rules\":{}}");
  CHECK(Contains(EncodePlayerMatchHost(6, {}, {}, 3), "\"slots\":3"));
  CHECK(EncodePlayerMatchJoin(7, "00112233445566aa") == "{\"op\":\"pm.join\",\"seq\":7,\"session\":\"00112233445566aa\"}");
  CHECK(EncodePlayerMatchSearch(1) == "{\"op\":\"pm.search\",\"seq\":1}");
  CHECK(EncodePlayerMatchCancel(1) == "{\"op\":\"pm.cancel\",\"seq\":1}");
  CHECK(Contains(EncodeFriendsAccept(1, "x"), "friends.accept") && Contains(EncodeFriendsDecline(1, "x"), "friends.decline") &&
        Contains(EncodeFriendsRemove(1, "x"), "friends.remove") && Contains(EncodeFriendsList(1), "friends.list") &&
        Contains(EncodeFriendsRequests(1), "friends.requests") && Contains(EncodePing(1), "\"op\":\"ping\""));
  const std::string endpoint = EncodeMatchEndpoint(6, 7001, {"127.0.0.1", "192.168.1.5"});
  CHECK(Contains(endpoint, "\"op\":\"match.endpoint\"") && !Contains(endpoint, "\"match\"") &&
        Contains(endpoint, "\"port\":7001") && Contains(endpoint, "\"lan\":[\"127.0.0.1\",\"192.168.1.5\"]") &&
        !Contains(endpoint, "public_ip") && !Contains(endpoint, "public_port"));
  // The STUN-mapped endpoint goes whole: its own port, never the local one.
  const std::string mapped = EncodeMatchEndpoint(6, 7001, {}, "179.113.149.63", 54114);
  CHECK(Contains(mapped, "\"port\":7001") && Contains(mapped, "\"public_ip\":\"179.113.149.63\"") &&
        Contains(mapped, "\"public_port\":54114"));
  CHECK(!Contains(EncodeMatchEndpoint(6, 7001, {}, "179.113.149.63", 0), "public_ip"));
  CHECK(!Contains(EncodeMatchEndpoint(6, 7001, {}, "", 54114), "public_port"));
  CHECK(Contains(EncodeMatchLeave(1), "match.leave"));
  PartySnapshot party;
  party.characters = {0, 2, 3, 0};
  party.size = 3;
  party.host = 2;
  CHECK(EncodePartyReport(9, party) == "{\"op\":\"party.report\",\"seq\":9,\"characters\":[0,2,3],\"host\":2}");
  CHECK(EncodeMatchTransport(9, 3, "relay") == "{\"op\":\"match.transport\",\"seq\":9,\"peer\":3,\"transport\":\"relay\"}");
  CHECK(EncodeMatchLink(2, 4, "connected") == "{\"op\":\"match.link\",\"seq\":2,\"peer\":4,\"state\":\"connected\"}");
  const std::string room_hello = EncodeMatchHello(5, "00ff00ff00ff00ff", 3, std::string(32, 'a'), 7);
  CHECK(Contains(room_hello, "\"op\":\"match.hello\"") && Contains(room_hello, "\"proto\":4") &&
        Contains(room_hello, "\"match\":\"00ff00ff00ff00ff\"") && Contains(room_hello, "\"slot\":3") &&
        Contains(room_hello, "\"relay\":true") && Contains(room_hello, "\"generation_seen\":7"));
  CHECK(EncodePartyCreate(1) == "{\"op\":\"party.create\",\"seq\":1}" && EncodePartyLeave(2) == "{\"op\":\"party.leave\",\"seq\":2}");
  CHECK(EncodePartyKick(3, "ABCD-2345") == "{\"op\":\"party.kick\",\"seq\":3,\"player\":\"ABCD-2345\"}");
  CHECK(EncodeInviteAccept(4, "i1") == "{\"op\":\"invite.accept\",\"seq\":4,\"invite\":\"i1\"}");
  CHECK(EncodeLobbyPublish(5, true) == "{\"op\":\"lobby.publish\",\"seq\":5,\"visible\":true}");
  CHECK(EncodeLobbyPublish(5, false) == "{\"op\":\"lobby.publish\",\"seq\":5,\"visible\":false}");
  CHECK(EncodeLobbyList(6) == "{\"op\":\"lobby.list\",\"seq\":6}");
  CHECK(EncodeLobbyList(6, int64_t(9)) == "{\"op\":\"lobby.list\",\"seq\":6,\"rev\":9}");
  CHECK(EncodeLobbyList(6, int64_t(-1)) == "{\"op\":\"lobby.list\",\"seq\":6}");
}

void TestAnswersDecode() {
  auto ok = DecodeServerMessage("{\"re\":7,\"code\":\"ok\",\"friend_code\":\"ABCD-2345\"}");
  CHECK(ok && ok->kind == ServerMessage::Kind::kResponse && ok->seq == 7 && ok->code == "ok" && ok->friend_code == "ABCD-2345");
  auto list = DecodeServerMessage(
      "{\"re\":8,\"code\":\"ok\",\"friends\":[{\"player\":\"ABCD-2345\",\"name\":\"Player\",\"display_name\":\"Player-A41C\","
      "\"online\":true,\"state\":\"party\",\"platform\":\"Linux\",\"version\":\"0.3.0\",\"guest\":\"other\",\"compatible\":false},"
      "{\"player\":\"EFGH-6789\",\"name\":\"B\",\"online\":false,\"state\":\"offline\"}]}");
  CHECK(list && list->friends && list->friends->size() == 2);
  CHECK(list && (*list->friends)[0].name == "Player-A41C" && (*list->friends)[0].state == FriendState::kParty &&
        !(*list->friends)[0].compatible && (*list->friends)[0].platform == "Linux" &&
        (*list->friends)[0].guest == "other");
  CHECK(list && (*list->friends)[1].name == "B" && !(*list->friends)[1].online &&
        (*list->friends)[1].state == FriendState::kOffline);
  auto hosted = DecodeServerMessage("{\"re\":3,\"code\":\"ok\",\"session\":\"00112233445566aa\"}");
  CHECK(hosted && hosted->session == "00112233445566aa" && !hosted->access);
  auto search = DecodeServerMessage(
      "{\"re\":4,\"code\":\"ok\",\"sessions\":[{\"session\":\"00112233445566aa\",\"player\":\"ABCD-2345\",\"name\":\"A\","
      "\"guest\":\"other\",\"build\":1,\"slots\":4,\"rules\":{\"rounds\":2,\"timer\":1},\"platform\":\"Windows\","
      "\"version\":\"0.2.2\"}]}");
  CHECK(search && search->sessions && search->sessions->size() == 1);
  CHECK(search && (*search->sessions)[0].guest == "other" && (*search->sessions)[0].slots == 4 &&
        (*search->sessions)[0].rules == (PlayerMatchRules{{"rounds", 2}, {"timer", 1}}));
  CHECK(!DecodeServerMessage("{\"re\":4,\"code\":\"ok\",\"sessions\":[{\"player\":\"A\"}]}")->sessions);
  auto error = DecodeServerMessage("{\"re\":2,\"code\":\"party_full\"}");
  CHECK(error && error->code == "party_full" && !error->access && !error->friends);
  auto lobby = DecodeServerMessage(
      "{\"re\":9,\"code\":\"ok\",\"players\":[{\"player\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Player\","
      "\"display_name\":\"Player-A41C\",\"version\":\"0.2.2\",\"state\":\"pending\"}],\"rev\":12,\"visible\":true}");
  CHECK(lobby && lobby->lobby_players && lobby->lobby_players->size() == 1);
  CHECK(lobby && (*lobby->lobby_players)[0].name == "Player-A41C" &&
        (*lobby->lobby_players)[0].player == "0123456789abcdef0123456789abcdef" &&
        (*lobby->lobby_players)[0].relation == "pending" && (*lobby->lobby_players)[0].version == "0.2.2" &&
        (*lobby->lobby_players)[0].compatible);
  CHECK(lobby && lobby->lobby_rev == 12 && lobby->lobby_visible && !lobby->lobby_unchanged);
  auto old_lobby = DecodeServerMessage(
      "{\"re\":9,\"code\":\"ok\",\"players\":[{\"player\":\"ABCD-2345\",\"name\":\"Player1\",\"state\":\"online\","
      "\"platform\":\"Windows\",\"version\":\"0.2.2\",\"guest\":\"base\",\"compatible\":false}],\"rev\":12,\"visible\":true}");
  CHECK(old_lobby && (*old_lobby->lobby_players)[0].name == "Player1" &&
        (*old_lobby->lobby_players)[0].state == FriendState::kOnline &&
        (*old_lobby->lobby_players)[0].relation.empty() && !(*old_lobby->lobby_players)[0].compatible);
  auto requests = DecodeServerMessage(
      "{\"re\":11,\"code\":\"ok\",\"incoming\":[{\"player\":\"ABCD-2345\",\"name\":\"Player\","
      "\"display_name\":\"Player-09F2\"}],\"outgoing\":[{\"player\":\"EFGH-6789\",\"name\":\"Player1\"}]}");
  CHECK(requests && requests->incoming && requests->incoming->size() == 1 &&
        (*requests->incoming)[0].name == "Player-09F2");
  CHECK(requests && requests->outgoing && requests->outgoing->size() == 1 &&
        (*requests->outgoing)[0].name == "Player1");
  auto unchanged = DecodeServerMessage("{\"re\":10,\"code\":\"ok\",\"unchanged\":true,\"rev\":12,\"visible\":false}");
  CHECK(unchanged && unchanged->lobby_unchanged && !unchanged->lobby_players && unchanged->lobby_rev == 12 &&
        unchanged->lobby_visible && !*unchanged->lobby_visible);
  auto bad_lobby = DecodeServerMessage("{\"re\":10,\"code\":\"ok\",\"players\":[{\"name\":\"A\"}]}");
  CHECK(bad_lobby && !bad_lobby->lobby_players);
  CHECK(DescribeServiceError("party_full") == "The party is full." && DescribeServiceError("no_party") == "Start a party first.");
}

void TestEventsDecode() {
  using Kind = ServerMessage::Kind;
  auto peer = DecodeServerMessage(
      "{\"ev\":\"match.peer\",\"slot\":2,\"name\":\"A\",\"ip\":\"203.0.113.9\","
      "\"port\":7001,\"lan\":[\"127.0.0.1\",\"192.168.1.9\"],\"same_network\":true}");
  CHECK(peer && peer->kind == Kind::kMatchPeer && peer->peer.slot == 2 && peer->peer.port == 7001 && peer->peer.lan.size() == 2 &&
        peer->peer.same_network);
  CHECK(peer->peer.public_ip.empty() && peer->peer.public_port == 0 && !peer->peer.service_forwards_public);
  const auto mapped_peer = DecodeServerMessage(
      "{\"ev\":\"match.peer\",\"slot\":3,\"ip\":\"2804:14d::1\",\"port\":7001,\"service_forwards_public\":true,"
      "\"public_ip\":\"177.18.65.229\",\"public_port\":57219}");
  CHECK(mapped_peer && mapped_peer->peer.ip == "2804:14d::1" && mapped_peer->peer.public_ip == "177.18.65.229" &&
        mapped_peer->peer.public_port == 57219 && mapped_peer->peer.service_forwards_public);
  for (const char* bad : {"\"public_ip\":\"177.18.65.229\",\"public_port\":0",
                          "\"public_ip\":\"177.18.65.229\",\"public_port\":70000",
                          "\"public_ip\":\"177.18.65.229\"", "\"public_port\":57219",
                          "\"public_ip\":\"\",\"public_port\":57219"}) {
    const auto partial = DecodeServerMessage(
        std::string("{\"ev\":\"match.peer\",\"slot\":2,\"ip\":\"1.2.3.4\",\"port\":7001,") + bad + "}");
    CHECK(partial && partial->peer.public_ip.empty() && partial->peer.public_port == 0);
  }
  CHECK(!DecodeServerMessage("{\"ev\":\"challenge\",\"player\":\"ABCD-2345\",\"name\":\"A\"}"));
  auto status = DecodeServerMessage("{\"ev\":\"friend.status\",\"friend\":{\"player\":\"ABCD-2345\",\"name\":\"Player\","
                                    "\"display_name\":\"Player-A41C\",\"online\":true,\"state\":\"in_game\"}}");
  CHECK(status && status->kind == Kind::kFriendStatus && status->friend_view.state == FriendState::kInGame &&
        status->friend_view.name == "Player-A41C");
  CHECK(DecodeServerMessage("{\"ev\":\"match.ended\",\"reason\":\"host_left\"}")->kind == Kind::kMatchEnded);
  const auto lost = DecodeServerMessage("{\"ev\":\"match.peer_lost\",\"slot\":3}");
  CHECK(lost && lost->kind == Kind::kMatchPeerLost && lost->slot == 3);
  CHECK(DecodeServerMessage("{\"ev\":\"match.peer_back\",\"slot\":3}")->kind == Kind::kMatchPeerBack);
  const auto left = DecodeServerMessage("{\"ev\":\"match.left\",\"slot\":4,\"reason\":\"disconnect\"}");
  CHECK(left && left->kind == Kind::kMatchLeft && left->slot == 4 && left->reason == "disconnect");
  const auto request = DecodeServerMessage(
      "{\"ev\":\"friend.request\",\"player\":\"ABCD-2345\",\"name\":\"Player\",\"display_name\":\"Player-A41C\"}");
  CHECK(request && request->kind == Kind::kFriendRequest && request->request.name == "Player-A41C");
  CHECK(DecodeServerMessage("{\"ev\":\"friend.request\",\"player\":\"ABCD-2345\",\"name\":\"A\"}")->request.name == "A");
  const auto added = DecodeServerMessage(
      "{\"ev\":\"friend.added\",\"friend\":{\"player\":\"ABCD-2345\",\"name\":\"Player\","
      "\"display_name\":\"Player-09F2\",\"online\":true,\"state\":\"online\"}}");
  CHECK(added && added->kind == Kind::kFriendAdded && added->friend_view.name == "Player-09F2");
  CHECK(DecodeServerMessage("{\"ev\":\"friend.offline\",\"player\":\"ABCD-2345\"}")->kind == Kind::kFriendOffline);
  CHECK(DecodeServerMessage("{\"ev\":\"friend.removed\",\"player\":\"ABCD-2345\"}")->kind == Kind::kFriendRemoved);
}

void TestMalformedMessagesAreRejected() {
  for (const char* text : {"", "x", "[]", "{}", "{\"re\":1}", "{\"ev\":5}", "{\"ev\":\"unknown\"}", "{\"ev\":\"match.peer\"}",
                           "{\"ev\":\"match.peer\",\"slot\":2,\"ip\":\"1.2.3.4\",\"port\":70000}",
                           "{\"ev\":\"match.peer\",\"slot\":5,\"ip\":\"1.2.3.4\",\"port\":7001}",
                           "{\"ev\":\"match.peer\",\"slot\":0,\"ip\":\"1.2.3.4\",\"port\":7001}",
                           "{\"ev\":\"match.left\"}", "{\"ev\":\"match.left\",\"slot\":9}",
                           "{\"ev\":\"match.transport\",\"a\":1,\"b\":9,\"transport\":\"relay\"}",
                           "{\"ev\":\"party.ended\"}", "{\"ev\":\"party.update\",\"party\":{}}",
                           "{\"ev\":\"challenge\"}", "{\"ev\":\"friend.status\",\"friend\":5}"}) {
    CHECK(!DecodeServerMessage(text));
  }
}

void TestFriendCodes() {
  CHECK(NormalizeFriendCode("abcd 2345") == "ABCD-2345");
  CHECK(NormalizeFriendCode("ABCD-2345") == "ABCD-2345");
  CHECK(NormalizeFriendCode("abcd2345") == "ABCD-2345");
  for (const char* bad : {"", "ABCD", "ABCD-23456", "ABCI-2345", "ABCO-2345", "ABC0-2345", "ABC1-2345", "ABCD-234!"}) {
    CHECK(NormalizeFriendCode(bad).empty());
  }
}

void TestPastedFriendCodesAreNormalized() {
  for (const char* pasted : {"ABCD-2345", "abcd-2345", "ABCD2345", "abcd2345", "ABCD 2345", " ABCD-2345\r\n", "\tabcd 2345 \n",
                             "  a b c d - 2 3 4 5  ", "CEQN-Z7SE\r\n"}) {
    const std::string code = NormalizeFriendCode(pasted);
    CHECK(code.size() == 9 && code[4] == '-' && NormalizeFriendCode(code) == code);
  }
  CHECK(NormalizeFriendCode("ceqn z7se\r\n") == "CEQN-Z7SE");
  CHECK(NormalizeFriendCode("SXFF8P9T") == "SXFF-8P9T");
}

void TestPartialAndMalformedTextIsNotACode() {
  for (const char* typed : {"A", "ABCD", "ABCD-", "ABCD-234", "ABC!-2345", "ABCD-23456", "ABCD\xC2\xA0" "2345", "abcd-1234",
                            "ABCI-OOOO", "\r\n"}) {
    CHECK(NormalizeFriendCode(typed).empty());
  }
}

void TestVersionCompatibility() {
  CHECK(kLobbyProtocol == 4 && kMaxParty == 4);
  CHECK(GuestsCompatible(kGuestBase, 1, kGuestBase, 1));
  CHECK(GuestsCompatible(kGuestOther, 1, kGuestOther, 1));
  CHECK(!GuestsCompatible(kGuestBase, 1, kGuestOther, 1));
  CHECK(!GuestsCompatible(kGuestOther, 1, kGuestBase, 1));
  CHECK(!GuestsCompatible(kGuestBase, 1, kGuestBase, 2));
  CHECK(DescribeServiceError("version_mismatch") == "Game versions do not match.");
}

void TestPeerAddressChoice() {
  const std::vector<std::string> mine = {"127.0.0.1", "192.168.1.5"};
  PeerInfo remote;
  remote.ip = "203.0.113.9";
  remote.lan = {"127.0.0.1", "10.0.0.4"};
  CHECK(ChoosePeerAddress(mine, remote) == "203.0.113.9");
  remote.same_network = true;
  CHECK(ChoosePeerAddress(mine, remote) == "10.0.0.4");
  remote.lan = {"127.0.0.1", "192.168.1.5"};
  CHECK(ChoosePeerAddress(mine, remote) == "127.0.0.1");
  remote.lan = {"127.0.0.1"};
  CHECK(ChoosePeerAddress(mine, remote) == "127.0.0.1");
}

}  // namespace


void TestInviteMessages() {
  using Kind = ServerMessage::Kind;
  CHECK(EncodeInviteSend(3, "ABCD-2345") == "{\"op\":\"invite.send\",\"seq\":3,\"player\":\"ABCD-2345\"}");
  CHECK(EncodeInviteDecline(4, "i1") == "{\"op\":\"invite.decline\",\"seq\":4,\"invite\":\"i1\"}");
  auto sent = DecodeServerMessage("{\"re\":3,\"code\":\"ok\",\"invite\":\"i1\"}");
  CHECK(sent && sent->invite_id == "i1");
  auto invite = DecodeServerMessage(
      "{\"ev\":\"invite\",\"invite\":\"i1\",\"from\":{\"player\":\"ABCD-2345\",\"name\":\"A\"},\"expires_ms\":120000}");
  CHECK(invite && invite->kind == Kind::kInvite && invite->invite && invite->invite->id == "i1" &&
        invite->invite->from_player == "ABCD-2345" && invite->invite->from_name == "A");
  CHECK(!DecodeServerMessage("{\"ev\":\"invite\",\"from\":{\"player\":\"x\"}}"));
  CHECK(!DecodeServerMessage("{\"ev\":\"invite\",\"invite\":\"i1\"}"));
  for (const char* reason : {"declined", "expired", "cancelled", "host_left", "party_full", "accepted_elsewhere", "accepted"}) {
    const auto ended = DecodeServerMessage(std::string("{\"ev\":\"invite.ended\",\"invite\":\"i1\",\"reason\":\"") + reason + "\"}");
    CHECK(ended && ended->kind == Kind::kInviteEnded && ended->invite_id == "i1" && ended->reason == reason);
  }
  CHECK(!DecodeServerMessage("{\"ev\":\"invite.ended\",\"reason\":\"expired\"}"));
}

// The party the service describes: one hidden match, up to four members with stable slots, the people still invited.
void TestPartyMessages() {
  using Kind = ServerMessage::Kind;
  const char* snapshot =
      "{\"match\":\"00ff00ff00ff00ff\",\"diag\":\"0123456789ab\",\"host\":\"ABCD-2345\",\"generation\":3,\"session\":\"\","
      "\"members\":[{\"slot\":1,\"player\":\"ABCD-2345\",\"name\":\"A\",\"platform\":\"Windows\",\"version\":\"0.2.2\",\"state\":\"connected\"},"
      "{\"slot\":3,\"player\":\"EFGH-6789\",\"name\":\"B\",\"platform\":\"Linux\",\"version\":\"0.2.2\",\"state\":\"connecting\"},"
      "{\"slot\":9,\"player\":\"XXXX-2222\",\"name\":\"C\"}],"
      "\"invited\":[{\"player\":\"JKLM-3456\",\"name\":\"D\"}]}";
  const auto update = DecodeServerMessage(std::string("{\"ev\":\"party.update\",\"party\":") + snapshot + "}");
  CHECK(update && update->kind == Kind::kPartyUpdate && update->party);
  const Party& party = *update->party;
  CHECK(party.match == "00ff00ff00ff00ff" && party.diag == "0123456789ab" && party.host == "ABCD-2345" && party.generation == 3);
  CHECK(party.members.size() == 2);  // a slot outside 1-4 is dropped
  CHECK(party.member(1) && party.member(1)->state == MemberState::kConnected && party.member(3)->platform == "Linux");
  CHECK(party.member(3)->state == MemberState::kConnecting && !party.member(2) && !party.member(4));
  CHECK(party.member_of("EFGH-6789") && party.member_of("EFGH-6789")->slot == 3 && !party.member_of("NOPE-2222"));
  CHECK(party.invited.size() == 1 && party.invited[0].player == "JKLM-3456");

  const auto created = DecodeServerMessage(
      "{\"re\":1,\"code\":\"ok\",\"match\":\"00ff00ff00ff00ff\",\"diag\":\"0123456789ab\",\"slot\":1,"
      "\"join_token\":\"" + std::string(32, 'c') + "\",\"generation\":0,\"party\":" + snapshot + "}");
  CHECK(created && created->access && created->access->slot == 1 && created->access->match == "00ff00ff00ff00ff" &&
        created->access->join_token.size() == 32 && created->access->party && created->access->party->members.size() == 2);
  CHECK(!DecodeServerMessage("{\"re\":1,\"code\":\"ok\",\"match\":\"m\",\"slot\":7}")->access);

  const auto ended = DecodeServerMessage("{\"ev\":\"party.ended\",\"match\":\"00ff00ff00ff00ff\",\"reason\":\"host_left\"}");
  CHECK(ended && ended->kind == Kind::kPartyEnded && ended->match_id == "00ff00ff00ff00ff" && ended->reason == "host_left");
  CHECK(std::string(MemberStateName(MemberState::kDisconnected)) == "disconnected");
}

// The relay's signalling: the client says it can relay, the service says what it can do, a relayed match carries its
// token, the host's transport choice reaches the joiner, and a relay that ends says why.
void TestRelaySignalling() {
  HelloFields hello{"base", 1, "ABCD-2345", "tok", "Saulo", "0.2.2", "Windows", true, true};
  CHECK(Contains(EncodeHello(1, hello), ",\"relay\":true"));
  hello.relay = false;
  CHECK(!Contains(EncodeHello(1, hello), "relay"));

  const auto greeted = DecodeServerMessage(
      "{\"re\":1,\"code\":\"ok\",\"friend_code\":\"ABCD-2345\",\"capabilities\":{\"protocol_version\":4,"
      "\"worker_build\":\"2026-10-31-party1\",\"public_endpoints\":true,\"relay_supported\":true,"
      "\"relay_max_payload\":1472}}");
  CHECK(greeted && greeted->capabilities && greeted->capabilities->announced);
  CHECK(greeted->capabilities->protocol_version == 4 && greeted->capabilities->worker_build == "2026-10-31-party1");
  CHECK(greeted->capabilities->public_endpoints && greeted->capabilities->relay_supported);
  CHECK(greeted->capabilities->relay_max_payload == 1472);
  const auto legacy = DecodeServerMessage("{\"re\":1,\"code\":\"ok\",\"friend_code\":\"ABCD-2345\"}");
  CHECK(legacy && !legacy->capabilities);
  CHECK(!ServiceCapabilities{}.relay_supported && !ServiceCapabilities{}.public_endpoints);

  // The room's answer to match.hello carries the relay token and the roster with each pair's transport.
  const auto room = DecodeServerMessage(
      "{\"re\":5,\"code\":\"ok\",\"generation\":1,\"relay_token\":4000000000,\"members\":["
      "{\"slot\":1,\"name\":\"A\",\"state\":\"connected\",\"links\":[{\"peer\":2,\"transport\":\"unset\",\"state\":\"connecting\"}]},"
      "{\"slot\":2,\"name\":\"B\",\"state\":\"connecting\"}]}");
  CHECK(room && room->relay_token == 4000000000u && room->generation == 1 && room->room_members && room->room_members->size() == 2);
  CHECK(room->room_members->front().links.size() == 1 && room->room_members->front().links[0].transport == "unset");
  for (const char* token : {"-1", "4294967296", "\"77\"", "0"}) {
    const auto odd = DecodeServerMessage(std::string("{\"re\":5,\"code\":\"ok\",\"relay_token\":") + token + "}");
    CHECK(odd && odd->relay_token == 0);
  }
  const auto members = DecodeServerMessage("{\"ev\":\"match.members\",\"generation\":4,\"members\":[{\"slot\":2,\"name\":\"B\",\"state\":\"connected\"}]}");
  CHECK(members && members->kind == ServerMessage::Kind::kMatchMembers && members->generation == 4 && members->room_members->size() == 1);

  const auto relay = DecodeServerMessage("{\"ev\":\"match.transport\",\"a\":2,\"b\":3,\"transport\":\"relay\",\"by\":2}");
  CHECK(relay && relay->kind == ServerMessage::Kind::kMatchTransport && relay->pair_a == 2 && relay->pair_b == 3 &&
        relay->pair_by == 2 && relay->transport == "relay");
  CHECK(DecodeServerMessage("{\"ev\":\"match.transport\",\"a\":1,\"b\":4,\"transport\":\"direct\",\"by\":1}")->transport == "direct");
  CHECK(!DecodeServerMessage("{\"ev\":\"match.transport\",\"a\":1,\"b\":4,\"transport\":\"tcp\"}"));
  CHECK(!DecodeServerMessage("{\"ev\":\"match.transport\",\"transport\":\"relay\"}"));
  const auto closed = DecodeServerMessage("{\"ev\":\"relay.closed\",\"peer\":3,\"reason\":\"peer_disconnected\"}");
  CHECK(closed && closed->kind == ServerMessage::Kind::kRelayClosed && closed->slot == 3 && closed->reason == "peer_disconnected");
  CHECK(DescribeServiceError("relay_unavailable") == "The connection through the online service is not available.");
}

int main() {
  std::printf("online protocol tests\n");
  TestRelaySignalling();
  TestRequestsAreWellFormed();
  TestAnswersDecode();
  TestInviteMessages();
  TestPartyMessages();
  TestEventsDecode();
  TestMalformedMessagesAreRejected();
  TestFriendCodes();
  TestPastedFriendCodesAreNormalized();
  TestPartialAndMalformedTextIsNotACode();
  TestVersionCompatibility();
  TestPeerAddressChoice();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
