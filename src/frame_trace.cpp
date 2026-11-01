// DEBUG_SAULO: short-lived trace of the game's per-frame tick. Stage events are recorded on the frame thread only;
// the dumper thread prints the stages that have not returned when a traced frame stalls.
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/logging.h>

#include "frame_trace.h"
#include "guest_memory.h"
#include "online_bridge.h"

namespace tmnt {
namespace {

constexpr size_t kRing = 256;
constexpr auto kStall = std::chrono::milliseconds(1500);

std::atomic<int> g_frames_left{0};
std::atomic<uint64_t> g_frame_no{0};
std::atomic<bool> g_in_frame{false};
std::atomic<int64_t> g_frame_start_ms{0};
std::atomic<uint32_t> g_ring_pos{0};
std::array<std::atomic<uint32_t>, kRing * 2> g_ring{};
std::atomic<bool> g_dumper_started{false};
std::atomic<PPCContext*> g_frame_ctx{nullptr};
std::atomic<uint8_t*> g_guest_base{nullptr};
thread_local bool t_tracing = false;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Record(uint32_t stage, bool enter) {
  const uint32_t at = g_ring_pos.fetch_add(1) % kRing;
  g_ring[at * 2].store(stage);
  g_ring[at * 2 + 1].store(enter ? 1 : 0);
}

void DumpGuestStack() {
  const PPCContext* ctx = g_frame_ctx.load();
  const uint8_t* base = g_guest_base.load();
  if (!ctx || !base) return;
  REXLOG_INFO("DEBUG_SAULO FrameTrace guest r1={:08X} lr={:08X} ctr={:08X} last_indirect={:08X} r3={:08X}", ctx->r1.u32,
              uint32_t(ctx->lr), ctx->ctr.u32, ctx->last_indirect_target, ctx->r3.u32);
  std::string chain;
  uint32_t sp = ctx->r1.u32;
  for (int depth = 0; depth < 24 && sp; ++depth) {
    const uint32_t back = LoadBE32(base, sp);
    if (back <= sp) break;
    chain += fmt::format(" {:08X}", LoadBE32(base, back - 8));
    sp = back;
  }
  REXLOG_INFO("DEBUG_SAULO FrameTrace guest return chain (innermost first):{}", chain);
}

void DumpStall() {
  const uint32_t end = g_ring_pos.load();
  const uint32_t begin = end > kRing ? end - uint32_t(kRing) : 0;
  std::array<uint32_t, 64> open{};
  size_t depth = 0;
  for (uint32_t i = begin; i < end; ++i) {
    const uint32_t stage = g_ring[(i % kRing) * 2].load();
    if (g_ring[(i % kRing) * 2 + 1].load()) {
      if (depth < open.size()) open[depth++] = stage;
    } else if (depth) {
      --depth;
    }
  }
  std::string text;
  for (size_t i = 0; i < depth; ++i) text += fmt::format(" {:08X}", open[i]);
  REXLOG_INFO("DEBUG_SAULO FrameTrace STALL frame={} not_returned:{}", g_frame_no.load(), depth ? text : " none (stall is in the frame body, between stages)");
  DumpGuestStack();
  const uint32_t tail = end > 12 ? end - 12 : 0;
  for (uint32_t i = tail; i < end; ++i) {
    REXLOG_INFO("DEBUG_SAULO FrameTrace last {} {:08X}", g_ring[(i % kRing) * 2 + 1].load() ? "ENTER" : "EXIT ",
                g_ring[(i % kRing) * 2].load());
  }
}

void Dumper() {
  uint64_t dumped = 0;
  while (g_frames_left.load() > 0 || g_in_frame.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!g_in_frame.load()) continue;
    const uint64_t frame = g_frame_no.load();
    if (frame != dumped && NowMs() - g_frame_start_ms.load() > kStall.count()) {
      dumped = frame;
      DumpStall();
    }
  }
  g_dumper_started = false;
}

struct StageScope {
  StageScope(uint32_t stage) : stage_(stage), on_(t_tracing) {
    if (on_) Record(stage_, true);
  }
  ~StageScope() {
    if (on_) Record(stage_, false);
  }
  uint32_t stage_;
  bool on_;
};

}  // namespace

void ArmFrameTrace(int frames) {
  if (g_frames_left.load() > 0 || g_in_frame.load()) return;
  g_frames_left = frames;
  REXLOG_INFO("DEBUG_SAULO FrameTrace armed frames={}", frames);
  bool expected = false;
  if (g_dumper_started.compare_exchange_strong(expected, true)) std::thread(Dumper).detach();
}

}  // namespace tmnt

REX_EXTERN(__imp__sub_82279648);
REX_HOOK_RAW(sub_82279648) {
  tmnt::NoteFrameTick();
  if (tmnt::g_frames_left.load() <= 0) {
    __imp__sub_82279648(ctx, base);
    return;
  }
  const uint64_t frame = ++tmnt::g_frame_no;
  tmnt::g_frame_start_ms = tmnt::NowMs();
  tmnt::g_frame_ctx = &ctx;
  tmnt::g_guest_base = base;
  tmnt::g_in_frame = true;
  tmnt::t_tracing = true;
  REXLOG_INFO("DEBUG_SAULO FrameTrace ENTER frame={}", frame);
  __imp__sub_82279648(ctx, base);
  tmnt::t_tracing = false;
  tmnt::g_in_frame = false;
  --tmnt::g_frames_left;
  REXLOG_INFO("DEBUG_SAULO FrameTrace EXIT frame={}", frame);
}

#define TMNT_FRAME_STAGE(addr)                    REX_EXTERN(__imp__sub_##addr);                  REX_HOOK_RAW(sub_##addr) {                        tmnt::StageScope scope(0x##addr);               __imp__sub_##addr(ctx, base);                 }

TMNT_FRAME_STAGE(82279418)
TMNT_FRAME_STAGE(824CB120)
TMNT_FRAME_STAGE(82364BC0)
TMNT_FRAME_STAGE(82358438)
TMNT_FRAME_STAGE(824DDC58)
TMNT_FRAME_STAGE(824DE5A8)
TMNT_FRAME_STAGE(8228E788)
TMNT_FRAME_STAGE(82229BE8)
TMNT_FRAME_STAGE(823160F0)
TMNT_FRAME_STAGE(8231AC78)
TMNT_FRAME_STAGE(82278418)
TMNT_FRAME_STAGE(82278110)
TMNT_FRAME_STAGE(82315D30)
TMNT_FRAME_STAGE(82316440)
TMNT_FRAME_STAGE(823363E0)
TMNT_FRAME_STAGE(82364B88)
TMNT_FRAME_STAGE(822A90C0)
TMNT_FRAME_STAGE(823178C0)
TMNT_FRAME_STAGE(822793A8)
TMNT_FRAME_STAGE(822F06E0)
TMNT_FRAME_STAGE(822786B0)
TMNT_FRAME_STAGE(822105F8)
TMNT_FRAME_STAGE(82272760)
TMNT_FRAME_STAGE(82271E30)
TMNT_FRAME_STAGE(82274010)
TMNT_FRAME_STAGE(82275B80)
TMNT_FRAME_STAGE(824B5C98)
TMNT_FRAME_STAGE(8226ACA0)
TMNT_FRAME_STAGE(82279260)
TMNT_FRAME_STAGE(82279318)
TMNT_FRAME_STAGE(82553630)
TMNT_FRAME_STAGE(824CB1F8)
