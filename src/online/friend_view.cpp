#include "online/friend_view.h"

#include <algorithm>
#include <cctype>

namespace tmnt::online {

std::string GuestLabel(const std::string& guest) {
  if (guest == kGuestBase) return "Base";
  std::string label;
  for (const char c : guest) label += char(std::toupper(static_cast<unsigned char>(c)));
  return label;
}

namespace {

const char* TransportLabel(MatchTransport transport) {
  switch (transport) {
    case MatchTransport::kDirect: return "Direct";
    case MatchTransport::kRelay: return "Relay";
    default: return "";
  }
}

std::string PeerStatus(const PeerView& peer) {
  if (peer.member == MemberState::kDisconnected) return "Reconnecting";
  if (peer.transport == MatchTransport::kFailed) return "Could not connect";
  const char* transport = TransportLabel(peer.transport);
  if (!*transport) return "Connecting";
  const std::string latency = peer.rtt_ms ? " \xC2\xB7 " + std::to_string(*peer.rtt_ms) + " ms" : std::string();
  return std::string("Connected \xC2\xB7 ") + transport + latency;
}

}  // namespace

std::vector<InviteChoice> InviteChoices(const std::vector<Friend>& friends) {
  std::vector<InviteChoice> choices;
  for (const Friend& f : friends) {
    InviteChoice choice{f.player, f.name, "Offline", false};
    if (f.online) {
      const bool busy = f.state == FriendState::kConnecting || f.state == FriendState::kInGame ||
                        f.state == FriendState::kParty || f.state == FriendState::kPlayerMatch;
      choice.status = f.state == FriendState::kParty || f.state == FriendState::kPlayerMatch ? "In a party"
                      : busy                                                                 ? "In game"
                                                                                             : "Online";
      if (!f.compatible) choice.status = "Game versions do not match";
      choice.selectable = f.compatible && !busy;
    }
    choices.push_back(std::move(choice));
  }
  std::stable_partition(choices.begin(), choices.end(), [](const InviteChoice& c) { return c.selectable; });
  return choices;
}

std::vector<InviteChoice> PartyInviteChoices(const Snapshot& state) {
  std::vector<InviteChoice> choices = InviteChoices(state.friends);
  const bool hosting = state.party && state.my_slot == 1;
  const size_t places = state.party ? state.party->members.size() + state.party->invited.size() : 0;
  const size_t capacity = state.party ? size_t(state.party->slots) : 0;
  for (InviteChoice& choice : choices) {
    if (!hosting) {
      choice.selectable = false;
      continue;
    }
    const bool member = state.party->member_of(choice.player) != nullptr;
    const bool invited = std::any_of(state.party->invited.begin(), state.party->invited.end(),
                                     [&](const PartyInvited& entry) { return entry.player == choice.player; });
    if (member || invited) {
      choice.status = member ? "In your party" : "Invited";
      choice.selectable = false;
    } else if (choice.selectable && places >= capacity) {
      choice.status = "Party full";
      choice.selectable = false;
    }
  }
  std::stable_partition(choices.begin(), choices.end(), [](const InviteChoice& c) { return c.selectable; });
  return choices;
}

FriendRowView DescribeFriend(const Snapshot& state, const Friend& entry) {
  FriendRowView view;
  view.status = FriendStatusLine(entry);
  if (state.party) {
    if (const PartyMember* member = state.party->member_of(entry.player)) {
      const auto peer = std::find_if(state.peers.begin(), state.peers.end(),
                                     [&](const PeerView& p) { return p.slot == member->slot; });
      view.status = peer != state.peers.end() ? "In your party \xC2\xB7 " + PeerStatus(*peer) : "In your party";
      return view;
    }
  }
  view.incompatible = entry.online && !entry.compatible;
  return view;
}

LobbyRowView DescribeLobbyPlayer(const Snapshot& state, const LobbyPlayer& player) {
  Friend shown;
  shown.player = player.player;
  shown.name = player.name;
  shown.online = true;
  shown.state = player.state;
  shown.platform = player.platform;
  shown.version = player.version;
  LobbyRowView view;
  view.status = FriendStatusLine(shown);
  if (!player.guest.empty()) view.status += " \xC2\xB7 " + GuestLabel(player.guest);
  if (!player.player.empty()) view.status += " \xC2\xB7 " + player.player;
  view.incompatible = !player.compatible;
  const auto is_player = [&player](const auto& entry) { return entry.player == player.player; };
  if (std::any_of(state.friends.begin(), state.friends.end(), is_player)) view.relation = LobbyRelation::kFriend;
  else if (std::any_of(state.outgoing.begin(), state.outgoing.end(), is_player)) view.relation = LobbyRelation::kOutgoing;
  else if (std::any_of(state.incoming.begin(), state.incoming.end(), is_player)) view.relation = LobbyRelation::kIncoming;
  view.can_add = view.relation == LobbyRelation::kNone && player.player != state.friend_code &&
                 state.connection == Connection::kOnline;
  return view;
}

std::string LobbyMessage(const LobbySnapshot& lobby) {
  if (!lobby.supported) return "The online service does not have the public Lobby yet.";
  if (lobby.failed && !lobby.fetched) return "Cannot read the Lobby right now.";
  if (!lobby.fetched) return "Looking for players...";
  if (lobby.players.empty()) return "No players are in the Lobby right now.";
  return {};
}

std::vector<PartySlotView> DescribePartySlots(const Snapshot& state) {
  std::vector<PartySlotView> slots(kMaxParty);
  for (int i = 0; i < kMaxParty; ++i) slots[i].slot = i + 1;
  if (!state.party) return slots;
  size_t next_invited = 0;
  for (PartySlotView& view : slots) {
    const PartyMember* member = state.party->member(view.slot);
    if (!member) {
      if (next_invited < state.party->invited.size()) {
        const PartyInvited& invited = state.party->invited[next_invited++];
        view.invited = true;
        view.name = invited.name.empty() ? invited.player : invited.name;
        view.status = "Invited";
      } else {
        view.status = "Empty";
      }
      continue;
    }
    view.occupied = true;
    view.name = member->name;
    view.host = view.slot == 1;
    view.you = view.slot == state.my_slot;
    if (view.you) {
      view.status = state.room_state == "online" ? "Connected" : "Connecting";
      continue;
    }
    const auto peer = std::find_if(state.peers.begin(), state.peers.end(), [&](const PeerView& p) { return p.slot == view.slot; });
    if (peer == state.peers.end()) {
      view.status = member->state == MemberState::kJoined         ? "Joining"
                    : member->state == MemberState::kDisconnected ? "Reconnecting"
                                                                  : "Connecting";
      continue;
    }
    view.status = peer->member == MemberState::kDisconnected                                      ? "Reconnecting"
                  : peer->transport == MatchTransport::kFailed                                    ? "Could not connect"
                  : peer->transport == MatchTransport::kDirect || peer->transport == MatchTransport::kRelay ? "Connected"
                                                                                                  : "Connecting";
    view.problem = peer->transport == MatchTransport::kFailed;
    view.transport = TransportLabel(peer->transport);
    view.ping_ms = peer->rtt_ms;
  }
  return slots;
}

std::string PartyHeadline(const Snapshot& state) {
  return state.party ? "Party " + std::to_string(state.party->members.size()) + "/" + std::to_string(kMaxParty) : std::string();
}

}  // namespace tmnt::online
