#include "online_bridge.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <rex/hook.h>
#include <rex/logging.h>

#include "netplay/relay_frame.h"
#include "netplay/udp_transport.h"
#include "online_runtime.h"
#include "player_match.h"
#include "player_match_net.h"

namespace tmnt {
namespace {

namespace np = netplay;

std::mutex g_lobby_mutex;
std::unique_ptr<np::LobbyMux> g_mux;
np::LobbyHold g_hold;
uint32_t g_generation = 0;
uint32_t g_tag = 0;

uint32_t CurrentTag(const std::vector<online::ActivePeer>& peers) {
  return peers.empty() ? 0 : SessionTag(peers.front().match);
}

std::optional<np::Endpoint> EndpointOf(const online::NetplayAssignment& assignment) {
  if (assignment.relay) return np::RelayPeerEndpoint(assignment.slot);
  uint32_t address = 0;
  if (!np::UsableAddress(np::ParsePeerAddress(assignment.peer_ip, address))) return std::nullopt;
  return np::Endpoint{address, assignment.peer_port};
}

// One mux per party generation on the gameplay socket; each participant joins it once its transport is selected.
void UpdateMatchTransport() {
  const std::vector<online::ActivePeer> peers = Online().Peers();
  const std::vector<online::NetplayAssignment> assignments = Online().Assignments();
  std::lock_guard<std::mutex> lock(g_lobby_mutex);
  if (peers.empty()) {
    if (g_mux) {
      g_mux.reset();
      g_hold.Clear();
      g_tag = 0;
      REXLOG_INFO("DEBUG_SAULO player_match lobby transport end generation={}", g_generation);
    }
    return;
  }
  const uint32_t tag = CurrentTag(peers);
  const uint32_t generation = assignments.empty() ? g_generation : assignments.front().generation;
  if (!g_mux || tag != g_tag || (!assignments.empty() && generation != g_generation)) {
    g_mux = std::make_unique<np::LobbyMux>(GameSocket());
    g_mux->SetMatch(tag);
    g_tag = tag;
    g_generation = generation;
    REXLOG_INFO("DEBUG_SAULO player_match lobby transport begin tag={:08X} generation={}", tag, generation);
  }
  for (const online::NetplayAssignment& assignment : assignments) {
    const std::optional<np::Endpoint> endpoint = EndpointOf(assignment);
    if (!endpoint) {
      REXLOG_INFO("DEBUG_SAULO player_match lobby transport bad_peer_address slot={}", assignment.slot);
      continue;
    }
    if (!g_mux->HasPeer(assignment.slot)) {
      g_mux->SetPeer(assignment.slot, *endpoint);
      const std::vector<np::LobbyDatagram> held = g_hold.Take(tag, assignment.slot);
      for (const np::LobbyDatagram& datagram : held) g_mux->SendLobby(assignment.slot, datagram);
      REXLOG_INFO("DEBUG_SAULO player_match peer routed slot={} transport={} held={}", assignment.slot,
                  assignment.relay ? "relay" : "direct", held.size());
    }
    Online().SetPeerLink(assignment.slot, g_mux->lobby_received_from(assignment.slot) ? online::NetplayLink::kConnected
                                                                                       : online::NetplayLink::kConnecting);
  }
  for (int slot = 1; slot <= np::kMaxSlot; ++slot) {
    if (!g_mux->HasPeer(slot)) continue;
    const bool assigned = std::any_of(assignments.begin(), assignments.end(),
                                      [slot](const online::NetplayAssignment& a) { return a.slot == slot; });
    if (!assigned) {
      g_mux->ClearPeer(slot);
      REXLOG_INFO("DEBUG_SAULO player_match peer unrouted slot={}", slot);
    }
  }
}

}  // namespace

bool SendPlayerMatchLobby(int slot, const np::LobbyDatagram& datagram) {
  std::lock_guard<std::mutex> lock(g_lobby_mutex);
  static int logged = 0;
  const bool sent = g_mux && g_mux->HasPeer(slot);
  if (logged < 6) {
    ++logged;
    REXLOG_INFO("DEBUG_SAULO lobby datagram slot={} path={} bytes={}", slot, sent ? "mux" : "held", datagram.data.size());
  }
  if (sent) return g_mux->SendLobby(slot, datagram);
  return g_tag && g_hold.Hold(g_tag, slot, datagram);
}

bool TakePlayerMatchLobby(int& slot, np::LobbyDatagram& out) {
  std::lock_guard<std::mutex> lock(g_lobby_mutex);
  return g_mux && g_mux->TakeLobby(slot, out);
}

std::atomic<uint64_t> g_frame_ticks{0};
std::atomic<uint64_t> g_input_ticks{0};
std::atomic<uint64_t> g_pad_reads{0};
std::atomic<uint32_t> g_last_buttons{0};

void LobbyHeartbeat(uint8_t* base) {
  static uint64_t frames = 0;
  static uint64_t last_game = 0;
  static uint64_t last_input = 0;
  static uint64_t last_pad = 0;
  static double last = 0;
  ++frames;
  const double now = OnlineNowSeconds();
  if (now - last < 1.0) return;
  last = now;
  const auto party = Online().party();
  if (!party) return;
  const uint64_t game = g_frame_ticks.load();
  const uint64_t input = g_input_ticks.load();
  const uint64_t pad = g_pad_reads.load();
  REXLOG_INFO("DEBUG_SAULO lobby heartbeat swaps={} game_ticks+{} input_ticks+{} pad_reads+{} buttons={:04X} party_members={} peers={} {}",
              frames, game - last_game, input - last_input, pad - last_pad, g_last_buttons.load(), party->members.size(),
              Online().Peers().size(), SessionHeartbeat(base));
  last_game = game;
  last_input = input;
  last_pad = pad;
}

void NoteFrameTick() { ++g_frame_ticks; }
void NoteInputTick() { ++g_input_ticks; }
void NotePadRead(uint16_t buttons) {
  ++g_pad_reads;
  g_last_buttons = buttons;
}

void PumpOnlineNetwork(uint8_t* base) {
  if (!OnlineRunning()) return;
  LobbyHeartbeat(base);
  PumpOnlineService();
  UpdateMatchTransport();
  CompleteVdpReceives(base);
}

}  // namespace tmnt

// DEBUG_SAULO: the controller-poll tick; the per-frame tick hook lives in frame_trace.cpp.

REX_EXTERN(__imp__sub_824BE610);
REX_HOOK_RAW(sub_824BE610) {
  tmnt::NoteInputTick();
  __imp__sub_824BE610(ctx, base);
}
