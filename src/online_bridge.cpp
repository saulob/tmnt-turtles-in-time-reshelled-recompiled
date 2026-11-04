#include "online_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include <rex/hook.h>
#include <rex/logging.h>

#include "frame_block_sampler.h"
#include "frame_trace.h"
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
    const std::optional<np::Endpoint> endpoint =
        np::LobbyPeerEndpoint(assignment.slot, assignment.relay, assignment.peer_ip, assignment.peer_port);
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
std::atomic<uint64_t> g_net_waits{0};
std::atomic<uint64_t> g_net_wait_ms{0};
std::atomic<uint64_t> g_frame_net_wait_ms{0};
std::atomic<uint64_t> g_select_wait_ms{0};
std::atomic<uint64_t> g_frame_select_wait_ms{0};
std::atomic<uint64_t> g_select_calls{0};
std::atomic<uint64_t> g_select_read{0};
std::atomic<uint64_t> g_select_write{0};
std::atomic<uint64_t> g_select_except{0};
std::atomic<uint64_t> g_tick_us{0};
std::atomic<uint64_t> g_pace_us{0};
std::atomic<uint64_t> g_swap_us{0};
std::atomic<size_t> g_shim_deepest{0};
std::atomic<uint64_t> g_shim_dropped{0};
std::atomic<uint64_t> g_pump_us{0};
std::atomic<uint64_t> g_packets_tx{0};
std::atomic<uint64_t> g_packets_rx{0};
thread_local bool t_frame_thread = false;

void LobbyHeartbeat(uint8_t* base) {
  static uint64_t frames = 0;
  static uint64_t last_game = 0;
  static uint64_t last_input = 0;
  static uint64_t last_pad = 0;
  static uint64_t last_waits = 0;
  static uint64_t last_wait_ms = 0;
  static uint64_t last_frame_wait_ms = 0;
  static uint64_t last_select_ms = 0;
  static uint64_t last_frame_select_ms = 0;
  static uint64_t last_select_calls = 0;
  static uint64_t last_select_read = 0;
  static uint64_t last_select_write = 0;
  static uint64_t last_select_except = 0;
  static uint64_t last_tick_us = 0;
  static uint64_t last_pace_us = 0;
  static uint64_t last_swap_us = 0;
  static uint64_t last_thread_cpu_us = 0;
  static uint64_t last_process_cpu_us = 0;
  static uint64_t last_shim_dropped = 0;
  static uint64_t last_pump_us = 0;
  static uint64_t last_tx = 0;
  static uint64_t last_rx = 0;
  static double last = 0;
  ++frames;
  const double now = OnlineNowSeconds();
  if (now - last < 1.0) return;
  last = now;
  const auto party = Online().party();
  if (!party) return;
  const uint64_t game = g_frame_ticks.load();
  constexpr uint64_t kSlowTicksPerSecond = 45;
  const uint64_t input = g_input_ticks.load();
  const uint64_t pad = g_pad_reads.load();
  const uint64_t waits = g_net_waits.load();
  const uint64_t wait_ms = g_net_wait_ms.load();
  const uint64_t frame_wait_ms = g_frame_net_wait_ms.load();
  const uint64_t select_ms = g_select_wait_ms.load();
  const uint64_t frame_select_ms = g_frame_select_wait_ms.load();
  const uint64_t pump_us = g_pump_us.load();
  const uint64_t tx = g_packets_tx.load();
  const uint64_t rx = g_packets_rx.load();
  const uint64_t select_calls = g_select_calls.load();
  const uint64_t select_read = g_select_read.load();
  const uint64_t select_write = g_select_write.load();
  const uint64_t select_except = g_select_except.load();
  const uint64_t tick_us = g_tick_us.load();
  const uint64_t pace_us = g_pace_us.load();
  const uint64_t swap_us = g_swap_us.load();
  const uint64_t thread_cpu_us = ThreadCpuMicroseconds();
  const uint64_t process_cpu_us = ProcessCpuMicroseconds();
  const uint64_t shim_dropped = g_shim_dropped.load();
  REXLOG_INFO("DEBUG_SAULO frame phases tick_ms+{} pace_ms+{} swap_ms+{} frame_cpu_ms+{} process_cpu_ms+{} select_calls+{} select_read+{} select_write+{} select_except+{} shim_queue_max={} shim_dropped+{} stages_ms={}",
              (tick_us - last_tick_us) / 1000, (pace_us - last_pace_us) / 1000, (swap_us - last_swap_us) / 1000,
              (thread_cpu_us - last_thread_cpu_us) / 1000, (process_cpu_us - last_process_cpu_us) / 1000,
              select_calls - last_select_calls, select_read - last_select_read, select_write - last_select_write,
              select_except - last_select_except, g_shim_deepest.exchange(0), shim_dropped - last_shim_dropped,
              FrameStageSummary());
  last_select_calls = select_calls;
  last_select_read = select_read;
  last_select_write = select_write;
  last_select_except = select_except;
  last_tick_us = tick_us;
  last_pace_us = pace_us;
  last_swap_us = swap_us;
  last_thread_cpu_us = thread_cpu_us;
  last_process_cpu_us = process_cpu_us;
  last_shim_dropped = shim_dropped;
  if (FrameSamplerEnabled()) {
    REXLOG_INFO("DEBUG_SAULO frame block {}", FrameBlockSummary());
    SetFrameSamplerActive(game - last_game < kSlowTicksPerSecond);
  }
  REXLOG_INFO("DEBUG_SAULO lobby heartbeat swaps={} game_ticks+{} input_ticks+{} pad_reads+{} buttons={:04X} party_members={} peers={} {} net_waits+{} net_wait_ms+{} frame_thread_net_wait_ms+{} select_wait_ms+{} frame_thread_select_wait_ms+{} pump_ms+{} packets_tx+{} packets_rx+{}",
              frames, game - last_game, input - last_input, pad - last_pad, g_last_buttons.load(), party->members.size(),
              Online().Peers().size(), SessionHeartbeat(base), waits - last_waits, wait_ms - last_wait_ms,
              frame_wait_ms - last_frame_wait_ms, select_ms - last_select_ms, frame_select_ms - last_frame_select_ms,
              (pump_us - last_pump_us) / 1000, tx - last_tx, rx - last_rx);
  last_select_ms = select_ms;
  last_frame_select_ms = frame_select_ms;
  last_pump_us = pump_us;
  last_tx = tx;
  last_rx = rx;
  last_waits = waits;
  last_wait_ms = wait_ms;
  last_frame_wait_ms = frame_wait_ms;
  last_game = game;
  last_input = input;
  last_pad = pad;
}

void NoteFrameTick() {
  t_frame_thread = true;
  ++g_frame_ticks;
}
void NoteNetWait(uint32_t milliseconds) {
  ++g_net_waits;
  g_net_wait_ms += milliseconds;
  if (t_frame_thread) g_frame_net_wait_ms += milliseconds;
}
void NoteSelect(uint32_t waited_milliseconds, size_t ready_read, size_t ready_write, size_t ready_except) {
  ++g_select_calls;
  g_select_read += ready_read;
  g_select_write += ready_write;
  g_select_except += ready_except;
  g_select_wait_ms += waited_milliseconds;
  if (t_frame_thread) g_frame_select_wait_ms += waited_milliseconds;
}
void NoteFrameTime(uint64_t microseconds) { g_tick_us += microseconds; }
void NotePresentPhases(uint64_t pace_microseconds, uint64_t swap_microseconds) {
  g_pace_us += pace_microseconds;
  g_swap_us += swap_microseconds;
}
void NoteShimQueues(size_t deepest, uint64_t dropped) {
  size_t seen = g_shim_deepest.load();
  while (deepest > seen && !g_shim_deepest.compare_exchange_weak(seen, deepest)) {
  }
  g_shim_dropped = dropped;
}
bool OnFrameThread() { return t_frame_thread; }
void NotePacketSent() { ++g_packets_tx; }
void NotePacketReceived() { ++g_packets_rx; }
void NoteInputTick() { ++g_input_ticks; }
void NotePadRead(uint16_t buttons) {
  ++g_pad_reads;
  g_last_buttons = buttons;
}

void PumpOnlineNetwork(uint8_t* base) {
  if (!OnlineRunning()) return;
  const auto started = std::chrono::steady_clock::now();
  LobbyHeartbeat(base);
  PumpOnlineService();
  UpdateMatchTransport();
  CompleteVdpReceives(base);
  g_pump_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}

}  // namespace tmnt

// DEBUG_SAULO: the controller-poll tick; the per-frame tick hook lives in frame_trace.cpp.

REX_EXTERN(__imp__sub_824BE610);
REX_HOOK_RAW(sub_824BE610) {
  tmnt::NoteInputTick();
  __imp__sub_824BE610(ctx, base);
}
