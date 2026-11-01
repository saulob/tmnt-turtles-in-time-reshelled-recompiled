#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "online/protocol.h"

namespace tmnt {

// The title sees an Xbox LIVE profile only while Online play is on and the online service has signed this player in.
constexpr bool LiveAvailable(bool online_enabled, bool service_signed_in) { return online_enabled && service_signed_in; }

// XUserGetSigninState: 2 = signed in to LIVE.
inline constexpr uint32_t kSignedInToLive = 2;
// XUSER_SIGNIN_INFO flags: online-enabled profile, never a guest.
inline constexpr uint32_t kSigninInfoOnlineEnabled = 0x1;

// The controller that owns the title-local LIVE profile. Other local players keep their offline profiles, so the
// game's own Live check keeps them out of an online match.
inline constexpr uint32_t kOnlineUser = 0;
constexpr bool LocalUser(uint32_t user) { return user < 4; }

// A stable online-style XUID (0x0009 prefix) derived only from the public friend code, never from the token.
uint64_t VirtualXuid(std::string_view friend_code);

// An online party is the host and up to three guests.
inline constexpr uint32_t kPlayerMatchSlots = 4;

// XSessionCreate flags.
inline constexpr uint32_t kSessionCreateHost = 0x1;

// XSESSION_INFO: XNKID (8), XNADDR (36), XNKEY (16).
inline constexpr uint32_t kSessionInfoBytes = 60;
inline constexpr uint32_t kXnkidBytes = 8;
// XSESSION_LOCAL_DETAILS: the host XNADDR follows the session id.
inline constexpr uint32_t kDetailsHostOffset = 64;
inline constexpr uint32_t kSearchHeaderBytes = 8;
inline constexpr uint32_t kSearchResultBytes = 92;

// The 8-byte session id from the service's 16 hex digits; zeros when malformed.
std::vector<uint8_t> SessionIdBytes(std::string_view session);
std::string SessionIdText(const uint8_t* bytes);

// Writes an XSESSION_SEARCHRESULT_HEADER and its results at guest address `header_address` (host view `header`),
// at most `max_results` of `ads`, each with its open public slots. Returns the results written;
// nothing is written when `capacity` cannot hold them.
uint32_t WriteSearchResults(uint8_t* header, uint32_t header_address, uint32_t capacity, uint32_t max_results,
                            const std::vector<online::PlayerMatchAd>& ads);

// XSessionSearchByID: the title requires 8 + 1326 bytes and one result in the usual search layout.
inline constexpr uint32_t kSearchByIdBytes = 1334;
bool WriteSearchByIdResult(uint8_t* header, uint32_t header_address, uint32_t capacity, const online::PlayerMatchAd& ad);

// XN_LIVE_INVITE_ACCEPTED: its parameter is the user index that accepted.
inline constexpr uint32_t kInviteAcceptedNotification = 0x02000002;
// XINVITE_INFO: invitee XUID, inviter XUID, title id, the host's XSESSION_INFO, fFromGameInvite.
inline constexpr uint32_t kInviteInfoBytes = 84;
inline constexpr uint32_t kTmntTitleId = 0x584109D2;
void WriteInviteInfo(uint8_t* info, uint64_t invitee, uint64_t inviter, std::string_view session);

// Called by the online service once an accepted invite's party is joined; tells the game to read the invite.
// DEBUG_SAULO: one-line state of the game-facing session for the lobby heartbeat.
std::string SessionHeartbeat(uint8_t* base);
void DeliverAcceptedInvite(const online::AcceptedInvite& invite);

// An XSESSION_INFO with only the session id; the host address is written separately.
void WriteSessionInfo(uint8_t* info, std::string_view session);

// What the game's XSessionGetDetails buffer holds (XSESSION_LOCAL_DETAILS): slot counts, the members the game itself
// joined to the session, the state, and the session id.
inline constexpr uint32_t kDetailsBytes = 128;
inline constexpr uint32_t kMemberBytes = 16;
inline constexpr uint32_t kRemoteUserIndex = 254;  // XUSER_INDEX_NONE: a member of another console
inline constexpr uint32_t kSessionStateLobby = 0;
inline constexpr uint32_t kSessionStateInGame = 2;
inline constexpr uint32_t kSessionStateReporting = 3;

struct SessionMember {
  uint64_t xuid = 0;
  uint32_t user_index = kRemoteUserIndex;
};

// XNKID (8) and XNKEY (16) as XNetCreateKey makes them: a system-link id and a random exchange key. The same entropy
// always gives the same pair.
inline constexpr uint32_t kNetKeyIdBytes = 8;
inline constexpr uint32_t kNetKeyBytes = 16;
void MakeNetKey(uint64_t entropy, uint8_t* id, uint8_t* key);

struct SessionDetails {
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint32_t state = kSessionStateLobby;
  std::string session;  // the id the game was given; its first 8 bytes fill the XNKID
  uint64_t host_xuid = 0;  // fills the host XNADDR; 0 leaves it zero
  std::vector<SessionMember> members;
};

// Fills `out` (at least kDetailsBytes) and the member array at `members_out` (kMaxParty entries); `members_address` is
// the member array's guest address. Slots are taken private first, as the host takes a private slot.
void WriteSessionDetails(uint8_t* out, uint8_t* members_out, uint32_t members_address, const SessionDetails& details);

}  // namespace tmnt
