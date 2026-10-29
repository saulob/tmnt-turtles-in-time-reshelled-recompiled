#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "live_scope.h"

using namespace tmnt;
using namespace tmnt::live;

namespace {

// A function in TMNT_r that nothing calls and that only returns true.
constexpr uint32_t kReturnsTrue = 0x8761B470;
constexpr int kMaxLoggedOverrides = 8;
constexpr uint32_t kWorldScriptOffset = 7216;

thread_local bool t_stats_requested = false;

bool HasLocalDisplayName(uint32_t user) {
  if (user < 1 || user > 3) return false;
  const std::string key = "local_player_" + std::to_string(user + 1) + "_name";
  return !rex::cvar::GetFlagByName(key).empty();
}

void LogLocalProfileAccepted(int32_t controller) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < kMaxLoggedOverrides) {
    REXLOG_INFO("[DEBUG_SAULO][Live] local profile accepted in {} (controller {})",
                t_live_scope_name ? t_live_scope_name : "?", controller);
  }
}

class FirstCall {
 public:
  explicit FirstCall(const char* name) : name_(name) {}
  void Note() {
    if (called_.load(std::memory_order_relaxed) || called_.exchange(true)) return;
    REXLOG_INFO("[DEBUG_SAULO][TMNT_r] first call: {}", name_);
  }

 private:
  const char* name_;
  std::atomic<bool> called_{false};
};

[[maybe_unused]] const bool g_hooks_linked = [] {
  REXLOG_INFO("[DEBUG_SAULO][TMNT_r] local hooks linked");
  return true;
}();

}  // namespace

// PLT_IsSignedIntoFullAccountOnline(controller in r5): the shared check behind every Live gate in the scripts.
REX_EXTERN(__imp__sub_876AE008);
REX_HOOK_RAW(sub_876AE008) {
  static FirstCall first_call("PLT_IsSignedIntoFullAccountOnline");
  first_call.Note();
  const int32_t controller = ctx.r5.s32;
  __imp__sub_876AE008(ctx, base);
  if (ctx.r3.u32 || !LocalProfileAccepted(controller)) return;
  ctx.r3.u64 = 1;
  LogLocalProfileAccepted(controller);
}

// Leaderboard_SwitchPage: refuses the page while the player is not online.
REX_EXTERN(__imp__sub_8759F858);
REX_HOOK_RAW(sub_8759F858) {
  static FirstCall first_call("Leaderboard_SwitchPage");
  first_call.Note();
  LiveScope scope("Leaderboard_SwitchPage");
  __imp__sub_8759F858(ctx, base);
}

// Fixed runtime slots sub_875A11B0 itself reads for its own completion call: the cached FireCommunicator script
// handle and the "Leaderboard_Main" track name pointer, both at a constant offset from lis r24/r11,-30866
// (0x876E0000), proven byte-exact against tmnt_r.image.bin.
constexpr uint32_t kFireUiHandleSlot = 0x876E112C;
constexpr uint32_t kLeaderboardMainNameSlot = 0x876E1128;

REX_EXTERN(sub_875B51D0);
REX_EXTERN(__imp__sub_87520018);

// Replays sub_875A11B0's own mi_PageStart>=mi_PageEnd completion: resolve FireCommunicator's "this" through the
// same two-step dispatch (import 1855 then 11) it uses, call FireCommunicator_DoWaitStop(this) directly exactly
// as it does, then SCR_TrackChange(0, "Leaderboard_Main") through import 1342.
void CompleteLeaderboardTrack(PPCContext& ctx, uint8_t* base, uint64_t lr) {
  const auto resolve_import = [&](uint32_t index) {
    ctx.r3.u64 = index;
    ctx.lr = lr;
    __imp__sub_87520018(ctx, base);
    return ctx.r3.u32;
  };
  bool resolved = true;
  const auto call = [&](uint32_t target) {
    PPCFunc* function = rex::runtime::ResolveIndirectFunction(target);
    if (!function) {
      resolved = false;
      return 0u;
    }
    ctx.lr = lr;
    function(ctx, base);
    return ctx.r3.u32;
  };

  const uint32_t resolve_this = resolve_import(1855);
  ctx.r3.u64 = LoadBE32(base, kFireUiHandleSlot);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  const uint32_t step = call(resolve_this);
  if (!resolved) return;

  const uint32_t resolve_method = resolve_import(11);
  ctx.r3.u64 = step;
  const uint32_t wait_stop_this = call(resolve_method);
  if (!resolved) return;

  ctx.r3.u64 = wait_stop_this;
  ctx.lr = lr;
  sub_875B51D0(ctx, base);

  const uint32_t track_change = resolve_import(1342);
  ctx.r3.u64 = 0;
  ctx.r4.u64 = LoadBE32(base, kLeaderboardMainNameSlot);
  call(track_change);
}

// Leaderboard_SendDataToFire(this): this+20 is the track's locals, +8 mi_PageStart and +12 mi_PageEnd (proven
// from generated/tmnt_r). The VM only ticks this function while that track is still active, so reaching this
// point at all already means the original did not transition away; once every requested row has gone through
// (mi_PageStart >= mi_PageEnd, including the zero-row case), fire its own completion path once more.
REX_EXTERN(__imp__sub_875A11B0);
REX_HOOK_RAW(sub_875A11B0) {
  static FirstCall first_call("Leaderboard_SendDataToFire");
  first_call.Note();
  const uint64_t lr = ctx.lr;
  const uint32_t vars = LoadBE32(base, ctx.r3.u32 + 20);
  __imp__sub_875A11B0(ctx, base);

  const uint32_t page_start = LoadBE32(base, vars + 8);
  const uint32_t page_end = LoadBE32(base, vars + 12);
  if (page_start < page_end) return;

  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < kMaxLoggedOverrides) {
    REXLOG_INFO("[DEBUG_SAULO][Leaderboard] forced completed leaderboard read start={} end={}", page_start, page_end);
  }
  CompleteLeaderboardTrack(ctx, base, lr);
  ctx.lr = lr;
}

// PLT_CheckPlayerSignIn(type): the deferred leaderboard sign-in; only the online type asks the shared check.
REX_EXTERN(__imp__sub_876AE720);
REX_HOOK_RAW(sub_876AE720) {
  static FirstCall first_call("PLT_CheckPlayerSignIn");
  first_call.Note();
  LiveScope scope("PLT_CheckPlayerSignIn");
  __imp__sub_876AE720(ctx, base);
}

// StartMenuWorld_CheckSignIn: leaves the online screen when the player is not online.
REX_EXTERN(__imp__sub_876B75C0);
REX_HOOK_RAW(sub_876B75C0) {
  static FirstCall first_call("StartMenuWorld_CheckSignIn");
  first_call.Note();
  LiveScope scope("StartMenuWorld_CheckSignIn");
  __imp__sub_876B75C0(ctx, base);
}

// FireCommunicator_OnReceiveEvent(this, var): one function for every UI event. Only the leaderboard proceed and the
// single- and local multiplayer proceeds open a scope; every other event, game loading included, stays offline.
REX_EXTERN(__imp__sub_875BA998);
REX_HOOK_RAW(sub_875BA998) {
  static FirstCall first_call("FireCommunicator_OnReceiveEvent");
  first_call.Note();
  const uint32_t var = ctx.r4.u32;
  const char* name = LocalOnlineEventName(LoadBE32(base, var), LoadBE32(base, var + kFireEventSourceOffset));
  if (name) {
    LiveScope scope(name);
    __imp__sub_875BA998(ctx, base);
    return;
  }
  LiveScopeSuspended offline;
  __imp__sub_875BA998(ctx, base);
}

// SessionWorld_RequestWriteStatsAllValid: only InGameHUD_GoShowSummary calls it.
REX_EXTERN(__imp__sub_875A6770);
REX_HOOK_RAW(sub_875A6770) {
  static FirstCall first_call("SessionWorld_RequestWriteStatsAllValid");
  first_call.Note();
  t_stats_requested = true;
  __imp__sub_875A6770(ctx, base);
}

REX_EXTERN(__imp__sub_87520018);

// The summary screen requests the stats write only when the platform reports a session, and an offline game has none.
// The request is the one the script makes: WOR_MDF_Script SessionWorld_RequestWriteStatsAllValid().
static void RequestStatsWrite(PPCContext& ctx, uint8_t* base, uint64_t lr) {
  const auto resolve_import = [&](uint32_t index) {
    ctx.r3.u64 = index;
    ctx.lr = lr;
    __imp__sub_87520018(ctx, base);
    return ctx.r3.u32;
  };
  bool resolved = true;
  const auto call = [&](uint32_t target) {
    PPCFunc* function = rex::runtime::ResolveIndirectFunction(target);
    if (!function) {
      resolved = false;
      return 0u;
    }
    ctx.lr = lr;
    function(ctx, base);
    return ctx.r3.u32;
  };

  const uint32_t world_script = resolve_import(5);
  ctx.r3.u64 = world_script;
  const uint32_t world = call(world_script);
  if (!resolved) return;
  const uint32_t handle = LoadBE32(base, world + kWorldScriptOffset);

  const uint32_t get_modifier = resolve_import(1855);
  ctx.r3.u64 = handle;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  const uint32_t modifier = call(get_modifier);
  if (!resolved) return;

  const uint32_t get_script = resolve_import(11);
  ctx.r3.u64 = modifier;
  call(get_script);
  if (!resolved) return;

  ctx.lr = lr;
  __imp__sub_875A6770(ctx, base);
}

// InGameHUD_GoShowSummary(this, var)
REX_EXTERN(__imp__sub_87599B80);
REX_HOOK_RAW(sub_87599B80) {
  static FirstCall first_call("InGameHUD_GoShowSummary");
  first_call.Note();
  const uint64_t lr = ctx.lr;
  t_stats_requested = false;
  __imp__sub_87599B80(ctx, base);
  if (t_stats_requested) return;

  REXLOG_INFO("[DEBUG_SAULO][Score] no platform session at the summary; requesting the stats write");
  RequestStatsWrite(ctx, base, lr);
  ctx.r3.u64 = 0;
  ctx.lr = lr;
}

// FireCommunicator_SendCharSelGamerTag(this, joy_id, slot_id, track_no): a controller that has a configured name is
// treated as signed in to the console, so the label shows that name and not "Player N".
REX_EXTERN(__imp__sub_875BA650);
REX_HOOK_RAW(sub_875BA650) {
  static FirstCall first_call("FireCommunicator_SendCharSelGamerTag");
  first_call.Note();
  if (!HasLocalDisplayName(ctx.r4.u32)) {
    __imp__sub_875BA650(ctx, base);
    return;
  }
  ForcedImport signed_in(kImportPlayerSignedIntoConsole);
  __imp__sub_875BA650(ctx, base);
}

// Engine import table lookup used by every compiled script call.
REX_HOOK_RAW(sub_87520018) {
  static FirstCall first_call("engine import lookup");
  first_call.Note();
  if (ImportForced(ctx.r3.u32)) {
    ctx.r3.u64 = kReturnsTrue;
    return;
  }
  __imp__sub_87520018(ctx, base);
}
