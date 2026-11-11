#include "online_bridge.h"

#include <algorithm>
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
#include "snes_gameplay_runtime.h"

namespace tmnt {
namespace {

namespace np = netplay;

constexpr int kMaxLoggedBadPeers = 8;

std::mutex g_lobby_mutex;
std::unique_ptr<np::LobbyMux> g_mux;
np::LobbyHold g_hold;
uint32_t g_generation = 0;
uint32_t g_tag = 0;

uint32_t CurrentTag(const std::vector<online::ActivePeer>& peers) {
  return peers.empty() ? 0 : SessionTag(peers.front().match);
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
      REXLOG_INFO("player_match lobby transport end generation={}", g_generation);
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
    REXLOG_INFO("player_match lobby transport begin tag={:08X} generation={}", tag, generation);
  }
  for (const online::NetplayAssignment& assignment : assignments) {
    const std::optional<np::Endpoint> endpoint =
        np::LobbyPeerEndpoint(assignment.slot, assignment.relay, assignment.peer_ip, assignment.peer_port);
    if (!endpoint) {
      static int logged = 0;
      if (logged++ < kMaxLoggedBadPeers) REXLOG_INFO("player_match lobby transport bad_peer_address slot={}", assignment.slot);
      continue;
    }
    if (!g_mux->HasPeer(assignment.slot)) {
      g_mux->SetPeer(assignment.slot, *endpoint);
      const std::vector<np::LobbyDatagram> held = g_hold.Take(tag, assignment.slot);
      for (const np::LobbyDatagram& datagram : held) g_mux->SendLobby(assignment.slot, datagram);
      REXLOG_INFO("player_match peer routed slot={} transport={} held={}", assignment.slot,
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
      REXLOG_INFO("player_match peer unrouted slot={}", slot);
    }
  }
}

}  // namespace

bool SendPlayerMatchLobby(int slot, const np::LobbyDatagram& datagram) {
  std::lock_guard<std::mutex> lock(g_lobby_mutex);
  const bool sent = g_mux && g_mux->HasPeer(slot);
  if (sent) return g_mux->SendLobby(slot, datagram);
  return g_tag && g_hold.Hold(g_tag, slot, datagram);
}

bool TakePlayerMatchLobby(int& slot, np::LobbyDatagram& out) {
  std::lock_guard<std::mutex> lock(g_lobby_mutex);
  return g_mux && g_mux->TakeLobby(slot, out);
}

thread_local bool t_frame_thread = false;

bool OnFrameThread() { return t_frame_thread; }

void PumpOnlineNetwork(uint8_t* base) {
  UpdateSnesOnline();
  if (!OnlineRunning()) return;
  PumpOnlineService();
  UpdateMatchTransport();
  CompleteVdpReceives(base);
}

}  // namespace tmnt

REX_EXTERN(__imp__sub_82279648);
REX_HOOK_RAW(sub_82279648) {
  tmnt::t_frame_thread = true;
  __imp__sub_82279648(ctx, base);
}
