#pragma once

namespace tmnt {

inline constexpr char kOnlineEnabledSetting[] = "tmnt_online_enabled";
inline constexpr bool kDefaultOnlineEnabled = false;
inline constexpr char kOnlineEnabledLabel[] = "Online play";
inline constexpr char kOnlineEnabledHelp[] =
    "Lets the game's own online menus create and join matches with your friends. Players meet through the online "
    "service; the game itself runs directly between your PCs whenever it can.";

inline constexpr char kOnlineNameSetting[] = "tmnt_online_name";
inline constexpr char kDefaultOnlineNameValue[] = "Player";
inline constexpr char kOnlineNameLabel[] = "Online name";
inline constexpr char kOnlineNameHelp[] = "What other players see: up to 16 letters, digits, - or _.";

inline constexpr char kOnlineLobbySetting[] = "tmnt_online_lobby_visible";
inline constexpr bool kDefaultOnlineLobby = false;
inline constexpr char kOnlineLobbyLabel[] = "Show me in Lobby";
inline constexpr char kOnlineLobbyOffHelp[] =
    "Turn on to let other players of this game see your Online name in the Lobby and send you a friend request.";
inline constexpr char kOnlineLobbyOnHelp[] =
    "Other players of this game can see your Online name, status, platform and game version in the Lobby and send you "
    "a friend request. Nothing else about you is shown.";

inline constexpr char kOnlinePingSetting[] = "tmnt_online_ping_display";
inline constexpr bool kDefaultOnlinePing = false;
inline constexpr char kOnlinePingLabel[] = "Ping display";
inline constexpr char kOnlinePingHelp[] =
    "Shows the round trip to the other player in game. F3 switches it while playing.";

// Developer only: another online service, for example the local one (server/tmnt-lobby/local/server.js).
inline constexpr char kOnlineServiceEnv[] = "TMNT_LOBBY";
// Developer only: STUN servers ("host:port,host:port" or "off") and UPnP ("0" or "1") of the Connection test.
inline constexpr char kStunServersEnv[] = "TMNT_STUN";
inline constexpr char kUpnpEnv[] = "TMNT_UPNP";

}  // namespace tmnt
