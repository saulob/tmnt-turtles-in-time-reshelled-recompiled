// DEBUG_SAULO: SCR_TrackChange(slot, name) is how the title's SessionManager and FireCommunicator scripts move
// between lobby steps, so the names are the lobby's operation transitions.
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "lobby_trace.h"
#include "online_runtime.h"

namespace {

constexpr uint32_t kMaxNameBytes = 96;

std::mutex g_mutex;
tmnt::RepeatFilter g_repeats;
tmnt::LineBudget g_budget(20);

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

REX_EXTERN(__imp__sub_822E8308);
REX_HOOK_RAW(sub_822E8308) {
  const uint32_t slot = ctx.r3.u32;
  const uint32_t name_address = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  if (tmnt::OnlineRunning() && name_address) {
    const char* text = reinterpret_cast<const char*>(tmnt::GuestHost(base, name_address));
    const std::string_view name(text, strnlen(text, kMaxNameBytes));
    if (tmnt::IsSessionTrack(name)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      const int repeats = g_repeats.Next(name);
      if (repeats >= 0 && g_budget.Allow(NowSeconds())) {
        if (repeats > 0) REXLOG_INFO("DEBUG_SAULO lobby track (previous change repeated {} more times)", repeats);
        if (const unsigned dropped = g_budget.TakeDropped()) {
          REXLOG_INFO("DEBUG_SAULO lobby track ({} changes dropped by the rate limit)", dropped);
        }
        REXLOG_INFO("DEBUG_SAULO lobby track slot={} name={} lr={:08X}", slot, name, lr);
      }
    }
  }
  __imp__sub_822E8308(ctx, base);
}
