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
// DEBUG_SAULO: time a thread spent blocked in the game's WSAWaitForMultipleEvents; the frame thread's share is shown.
// The address the game uses for `xuid` in the current room.
uint32_t ActiveSyntheticAddress(uint64_t xuid);
void NoteNetWait(uint32_t milliseconds);
void NoteSelect(uint32_t waited_milliseconds, size_t ready_read, size_t ready_write, size_t ready_except);
// DEBUG_SAULO: the phases of a frame, summed for the heartbeat.
void NoteFrameTime(uint64_t microseconds);
void NotePresentPhases(uint64_t pace_microseconds, uint64_t swap_microseconds);
void NoteShimQueues(size_t deepest, uint64_t dropped);
bool OnFrameThread();
void NotePacketSent();
void NotePacketReceived();

// Once per presented frame: the online service, each participant's transport, and the game's pending receives.
void PumpOnlineNetwork(uint8_t* base);

// Completes the game's pending overlapped receives that have a datagram waiting (player_match_net_hook.cpp).
void CompleteVdpReceives(uint8_t* base);
// DEBUG_SAULO: a few socket and start-handshake lines from XSessionStart on.
void ArmStartTrace();

}  // namespace tmnt
