#include "achievement_menu.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

#include <rex/hook.h>

#include "achievement_menu_rules.h"
#include "cheats.h"
#include "guest_memory.h"

namespace tmnt {
namespace {

// X_INPUT_STATE: a 32-bit packet number followed by the 12-byte gamepad.
constexpr uint32_t kGamepadOffset = 4;
constexpr uint32_t kGamepadSize = 12;
constexpr uint32_t kThumbLeftYOffset = 6;

std::mutex g_gate_mutex;
AchievementMenuGate g_gate;
std::atomic<bool> g_holds_input{false};
std::atomic<float> g_scroll_axis{0.0f};
AchievementMenuBridge g_bridge;

void FilterGuestInput(uint8_t* base, uint32_t user, uint32_t state) {
  if (!state) return;
  const uint32_t gamepad = state + kGamepadOffset;
  const uint16_t buttons = LoadBE16(base, gamepad);
  const int thumb_y = static_cast<int16_t>(LoadBE16(base, gamepad + kThumbLeftYOffset));

  AchievementMenuGate::Verdict verdict;
  {
    std::lock_guard lock(g_gate_mutex);
    verdict = g_gate.Poll(user, buttons, thumb_y);
    g_holds_input.store(g_gate.HoldsInput(), std::memory_order_release);
  }
  g_scroll_axis.store(verdict.scroll, std::memory_order_relaxed);

  if (verdict.close && g_bridge.close) g_bridge.close();
  if (verdict.hide_input) {
    std::memset(base + gamepad, 0, kGamepadSize);
  } else if (verdict.clear_b) {
    StoreBE16(base, gamepad, static_cast<uint16_t>(buttons & ~kXinputButtonB));
  }
}

}  // namespace

void InstallAchievementMenu(AchievementMenuBridge bridge) {
  g_bridge = std::move(bridge);
  REXLOG_INFO("[Achievement] Local achievements menu bridge installed");
}

void SetAchievementMenuOpen(bool open) {
  {
    std::lock_guard lock(g_gate_mutex);
    g_gate.SetOpen(open);
    g_holds_input.store(g_gate.HoldsInput(), std::memory_order_release);
  }
  g_scroll_axis.store(0.0f, std::memory_order_relaxed);
}

bool AchievementMenuHoldsInput() { return g_holds_input.load(std::memory_order_acquire); }

float AchievementMenuScrollAxis() { return g_scroll_axis.load(std::memory_order_relaxed); }

}  // namespace tmnt

using namespace tmnt;

// sub_826EDB50 is the game's XamShowAchievementsUI wrapper; ReXGlue stubs the import it calls.
REX_EXTERN(__imp__sub_826EDB50);
REX_HOOK_RAW(sub_826EDB50) {
  if (!g_bridge.open) {
    __imp__sub_826EDB50(ctx, base);
    return;
  }
  REXLOG_INFO("[Achievement] XamShowAchievementsUI wrapper: opening local overlay");
  g_bridge.open();
  ctx.r3.u64 = 0;
}

// sub_826EF030 is the game's only controller read (XamInputGetState with flags 1).
REX_EXTERN(__imp__sub_826EF030);
REX_HOOK_RAW(sub_826EF030) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_826EF030(ctx, base);
  FilterGuestInput(base, user, state);
  if (ctx.r3.u32 == 0) cheats::ApplyTurboAttack(base, user, state);
}
