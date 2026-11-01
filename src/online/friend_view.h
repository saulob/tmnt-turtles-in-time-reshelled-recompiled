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

// A row of the party invite list: every friend, with only the ones who can receive an invite selectable.
struct InviteChoice {
  std::string player;
  std::string name;
  std::string status;  // Online, Online Match, In game, Offline, or why it cannot be chosen
  bool selectable = false;
};
std::vector<InviteChoice> InviteChoices(const std::vector<Friend>& friends);

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
