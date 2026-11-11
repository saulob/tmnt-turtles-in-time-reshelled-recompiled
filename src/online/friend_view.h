#pragma once

#include <optional>
#include <string>
#include <vector>

#include "online/online_service.h"

namespace tmnt::online {

// "Base" for this port's game image; any other image id in capitals.
std::string GuestLabel(const std::string& guest);

struct FriendRowView {
  std::string status;  // the line under the friend's name
  bool incompatible = false;
};

// What one friend's row shows, given what this player is doing right now. Matches start from the game's own
// Multiplayer > Player Match, so a row offers no match controls. A latency appears only once the gameplay connection
// has measured one.
FriendRowView DescribeFriend(const Snapshot& state, const Friend& entry);

// What the Lobby page does with a listed player, from this player's own friends and requests: Add friend only while no
// relationship exists, so the Lobby and the Friends tab never disagree.
enum class LobbyRelation { kNone, kFriend, kOutgoing, kIncoming };

struct LobbyRowView {
  std::string status;  // the line under the player's name
  bool incompatible = false;
  LobbyRelation relation = LobbyRelation::kNone;
  bool can_add = false;
};

LobbyRowView DescribeLobbyPlayer(const Snapshot& state, const LobbyPlayer& player);

// "v1.1.1", or "Version unknown" when the client did not advertise one.
inline std::string LobbyVersionLabel(const LobbyPlayer& player) {
  return player.version.empty() ? "Version unknown" : "v" + player.version;
}
inline std::string LobbyUpdatedText(int minutes) {
  return minutes < 1 ? "Updated just now" : "Updated " + std::to_string(minutes) + " min ago";
}

struct LobbyAction {
  const char* text = nullptr;
  bool enabled = false;
  bool primary = false;
  bool decline = false;
};
inline LobbyAction LobbyActionOf(LobbyRelation relation) {
  switch (relation) {
    case LobbyRelation::kNone: return {"Send friend request", true, false};
    case LobbyRelation::kOutgoing: return {"Pending", false, false};
    case LobbyRelation::kFriend: return {"Friend", false, false};
    case LobbyRelation::kIncoming: return {"Accept", true, true, true};
  }
  return {};
}

// The Lobby rows this build can play with; internal incompatibility data never reaches the visible list.
inline std::vector<const LobbyPlayer*> VisibleLobbyPlayers(const LobbySnapshot& lobby) {
  std::vector<const LobbyPlayer*> players;
  for (const LobbyPlayer& player : lobby.players) {
    if (player.compatible) players.push_back(&player);
  }
  return players;
}

// What the Lobby shows in place of a list; empty when it shows one.
inline std::string LobbyMessage(const LobbySnapshot& lobby) {
  if (!lobby.supported) return "The Lobby is not available right now.";
  if (lobby.failed) return "Could not refresh the Lobby.";
  return {};
}

// A row of the party invite list: every friend, with only the ones who can receive an invite selectable.
struct InviteChoice {
  std::string player;
  std::string name;
  std::string status;  // Online, Online Match, In game, Offline, or why it cannot be chosen
  bool selectable = false;
};
std::vector<InviteChoice> InviteChoices(const std::vector<Friend>& friends);

// The in-game invite list of a hosted party: friends already in the party or invited, and every friend once the
// session's places are taken, are shown but cannot be chosen. A guest or a player without a party cannot choose anyone.
std::vector<InviteChoice> PartyInviteChoices(const Snapshot& state);

// One of the four player cards of the party, always four, empty ones included.
struct PartySlotView {
  int slot = 0;
  bool occupied = false;
  bool you = false;
  bool host = false;
  bool invited = false;
  std::string name;
  std::string status;     // Connected, Connecting, Joining, Reconnecting, Could not connect, Invited, Empty
  std::string transport;  // Direct, Relay, or empty
  std::optional<int> ping_ms;
  bool problem = false;
};
std::vector<PartySlotView> DescribePartySlots(const Snapshot& state);
// "Party 3/4", or empty without a party.
std::string PartyHeadline(const Snapshot& state);

}  // namespace tmnt::online
