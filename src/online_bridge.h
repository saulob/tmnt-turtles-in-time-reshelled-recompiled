#pragma once

#include <cstdint>

#include "netplay/lobby_mux.h"

namespace tmnt {

// The game's datagrams to and from the other participants of the party's match, over the online gameplay socket
// (directly or through the online service's relay, per participant). Datagrams sent before a participant's transport is
// selected are held, not lost.
bool SendPlayerMatchLobby(int slot, const netplay::LobbyDatagram& datagram);
bool TakePlayerMatchLobby(int& slot, netplay::LobbyDatagram& out);

// DEBUG_SAULO: game-side tick counters shown by the lobby heartbeat.
void NoteFrameTick();
void NoteInputTick();
void NotePadRead(uint16_t buttons);

// Once per presented frame: the online service, each participant's transport, and the game's pending receives.
void PumpOnlineNetwork(uint8_t* base);

// Completes the game's pending overlapped receives that have a datagram waiting (player_match_net_hook.cpp).
void CompleteVdpReceives(uint8_t* base);

}  // namespace tmnt
