#include <atomic>
#include <functional>
#include <thread>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "gamer_card_rules.h"
#include "guest_memory.h"
#include "live_scope.h"

using namespace tmnt;
using namespace tmnt::live;

namespace {

// A function in TMNT_r that nothing calls and that only returns true.
constexpr uint32_t kReturnsTrue = 0x8761B470;
constexpr int kMaxLoggedOverrides = 8;
constexpr uint32_t kWorldScriptOffset = 7216;
constexpr uint32_t kPltSucceed = 0;

thread_local bool t_stats_requested = false;

bool HasLocalDisplayName(uint32_t user) {
  if (user < 1 || user > 3) return false;
  const std::string key = "local_player_" + std::to_string(user + 1) + "_name";
  return !rex::cvar::GetFlagByName(key).empty();
}

void LogLocalProfileAccepted(int32_t controller) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < kMaxLoggedOverrides) {
    REXLOG_INFO("[Live] local profile accepted in {} (controller {})",
                t_live_scope_name ? t_live_scope_name : "?", controller);
  }
}

}  // namespace

// PLT_IsSignedIntoFullAccountOnline(controller in r5): the shared check behind every Live gate in the scripts.
REX_EXTERN(__imp__sub_876AE008);
REX_HOOK_RAW(sub_876AE008) {
  const int32_t controller = ctx.r5.s32;
  __imp__sub_876AE008(ctx, base);
  if (ctx.r3.u32 || !LocalProfileAccepted(controller)) return;
  ctx.r3.u64 = 1;
  LogLocalProfileAccepted(controller);
}

// Leaderboard_SwitchPage: refuses the page while the player is not online.
REX_EXTERN(__imp__sub_8759F858);
REX_HOOK_RAW(sub_8759F858) {
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
  const uint64_t lr = ctx.lr;
  const uint32_t vars = LoadBE32(base, ctx.r3.u32 + 20);
  __imp__sub_875A11B0(ctx, base);

  const uint32_t page_start = LoadBE32(base, vars + 8);
  const uint32_t page_end = LoadBE32(base, vars + 12);
  if (page_start < page_end) return;

  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < kMaxLoggedOverrides) {
    REXLOG_INFO("[Leaderboard] forced completed leaderboard read start={} end={}", page_start, page_end);
  }
  CompleteLeaderboardTrack(ctx, base, lr);
  ctx.lr = lr;
}

// PLT_CheckPlayerSignIn(type): the deferred leaderboard sign-in; only the online type asks the shared check.
REX_EXTERN(__imp__sub_876AE720);
REX_HOOK_RAW(sub_876AE720) {
  LiveScope scope("PLT_CheckPlayerSignIn");
  __imp__sub_876AE720(ctx, base);
}

// StartMenuWorld_CheckSignIn: leaves the online screen when the player is not online.
REX_EXTERN(__imp__sub_876B75C0);
REX_HOOK_RAW(sub_876B75C0) {
  LiveScope scope("StartMenuWorld_CheckSignIn");
  __imp__sub_876B75C0(ctx, base);
}

// FireCommunicator_OnReceiveEvent(this, var): one function for every UI event. Only the leaderboard proceed and the
// single- and local multiplayer proceeds open a scope; every other event, game loading included, stays offline.
REX_EXTERN(__imp__sub_875BA998);
REX_HOOK_RAW(sub_875BA998) {
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

// DEBUG_SAULO: FIRE wait request protocol. DoWait/DoWaitStop only post go_wait/go_stop; the deferred-send track
// consumes them on a later tick and is_waiting follows the overlay.
constexpr uint32_t kFireIsWaitingOffset = 0x30000 + 11824;
constexpr uint32_t kFireGoWaitOffset = 0x30000 + 11828;
constexpr uint32_t kFireGoStopOffset = 0x30000 + 11832;
constexpr uint32_t kFireWaitIdOffset = 0x30000 + 11836;

struct FireWaitState {
  uint32_t is_waiting, go_wait, go_stop, id;
};

FireWaitState ReadFireWait(uint8_t* base, uint32_t fire) {
  const uint32_t vars = fire ? LoadBE32(base, fire + 8) : 0;
  if (!vars) return {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  return {LoadBE32(base, vars + kFireIsWaitingOffset), LoadBE32(base, vars + kFireGoWaitOffset),
          LoadBE32(base, vars + kFireGoStopOffset), LoadBE32(base, vars + kFireWaitIdOffset)};
}

uint32_t ThreadTag() { return uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF); }

std::atomic<int> g_step_budget{200};
std::atomic<uint64_t> g_fire_ticks{0};
std::atomic<int> g_main_tick_trace{0};

void LogStep(const char* what, const char* phase, uint8_t* base, uint32_t fire, uint32_t arg, uint32_t lr) {
  if (g_step_budget.fetch_sub(1) <= 0) return;
  const FireWaitState s = ReadFireWait(base, fire);
  REXLOG_INFO("DEBUG_SAULO CreateMatchStep {} {} arg={} lr={:08X} is_waiting={} go_wait={} go_stop={} wait_id={} tid={} fire_tick={}",
              what, phase, arg, lr, s.is_waiting, s.go_wait, s.go_stop, s.id, ThreadTag(),
              g_fire_ticks.load());
}

#define TMNT_FIRE_STEP_HOOK(sub, label)                                  REX_EXTERN(__imp__sub_##sub);                                          REX_HOOK_RAW(sub_##sub) {                                                const uint32_t fire = ctx.r3.u32;                                      const uint32_t arg = ctx.r4.u32;                                       const uint32_t lr = uint32_t(ctx.lr);                                  LogStep(label, "ENTER", base, fire, arg, lr);                          __imp__sub_##sub(ctx, base);                                           LogStep(label, "EXIT", base, fire, arg, lr);                         }

// FireCommunicator_SetPkg(this, id, int values, string pointers): every UI event the game hands to the FIRE movie.
std::atomic<int> g_pkg_budget{400};

std::string PkgText(uint8_t* base, uint32_t strings) {
  if (!strings) return {};
  const uint32_t pointer = LoadBE32(base, strings);
  if (pointer < 0x10000 || pointer >= 0xA0000000) return {};
  const char* text = reinterpret_cast<const char*>(GuestHost(base, pointer));
  std::string out;
  for (size_t i = 0; i < 40 && text[i]; ++i) out += (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
  return out;
}

REX_EXTERN(__imp__sub_875ADBC0);
REX_HOOK_RAW(sub_875ADBC0) {
  const uint32_t id = ctx.r4.u32;
  const uint32_t ints = ctx.r5.u32;
  const uint32_t strings = ctx.r6.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  if (g_pkg_budget.fetch_sub(1) > 0 && ints) {
    REXLOG_INFO("DEBUG_SAULO FirePkg id={} ints={},{},{},{} str=\"{}\" lr={:08X} fire_tick={}", id, LoadBE32(base, ints),
                LoadBE32(base, ints + 4), LoadBE32(base, ints + 8), LoadBE32(base, ints + 12), PkgText(base, strings), lr,
                g_fire_ticks.load());
  }
  __imp__sub_875ADBC0(ctx, base);
}

// FireCommunicator_DoWait(this, wait_id)
TMNT_FIRE_STEP_HOOK(875B5138, "DoWait")

// FireCommunicator_DoWaitStop(this)
REX_EXTERN(__imp__sub_875B51D0);
REX_HOOK_RAW(sub_875B51D0) {
  const uint32_t fire = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  LogStep("DoWaitStop", "ENTER", base, fire, 0, lr);
  __imp__sub_875B51D0(ctx, base);
  LogStep("DoWaitStop", "AFTER_ORIGINAL/EXIT", base, fire, 0, lr);
  g_main_tick_trace.store(3);
}

TMNT_FIRE_STEP_HOOK(875B5288, "UpdateWaitDefered")
TMNT_FIRE_STEP_HOOK(875B5980, "UpdateWaitStopDefered")
TMNT_FIRE_STEP_HOOK(875B55F0, "UpdateWaitMsgDefered")
TMNT_FIRE_STEP_HOOK(875B53D8, "UpdateWait")
TMNT_FIRE_STEP_HOOK(875B5AD0, "UpdateWaitStop")

// x_FireCommunicator_DferredSendCheck tick: logged only while a wait request is pending.
REX_EXTERN(__imp__sub_875C1DC8);
REX_HOOK_RAW(sub_875C1DC8) {
  const uint32_t fire = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  ++g_fire_ticks;
  const FireWaitState before = ReadFireWait(base, fire);
  const bool pending = before.go_wait == 1 || before.go_stop == 1;
  if (pending) LogStep("DferredSendCheck", "ENTER", base, fire, 0, lr);
  __imp__sub_875C1DC8(ctx, base);
  if (pending) LogStep("DferredSendCheck", "EXIT", base, fire, ctx.r3.u32, lr);
}

// SessionManager main tick: the three ticks after DoWaitStop must return.
REX_EXTERN(__imp__sub_875D9290);
REX_HOOK_RAW(sub_875D9290) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool trace = g_main_tick_trace.load() > 0;
  if (trace) LogStep("SessionManagerMainTick", "ENTER", base, 0, 0, lr);
  __imp__sub_875D9290(ctx, base);
  if (trace) {
    g_main_tick_trace.fetch_sub(1);
    LogStep("SessionManagerMainTick", "EXIT", base, 0, ctx.r3.u32, lr);
  }
}

// StartMenuWorld_RegisterLocalPlayerInGame(joy_id, slot_id): warns a secondary controller without a console sign-in
// that scores will not reach the Xbox Live leaderboards.
REX_EXTERN(__imp__sub_876B6D08);
REX_HOOK_RAW(sub_876B6D08) {
  LocalPlayerJoin joining;
  __imp__sub_876B6D08(ctx, base);
}

// FireCommunicator_SendNotificationOverlayDefered(this, error_id in r4, joy_id, ...)
REX_EXTERN(__imp__sub_875BA888);
REX_HOOK_RAW(sub_875BA888) {
  if (DropsNotification(ctx.r4.u32)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < kMaxLoggedOverrides) {
      REXLOG_INFO("[Live] dropped LOC_NOTF_LEADERBRD_NOT_XLA for controller {}", ctx.r5.s32);
    }
    return;
  }
  __imp__sub_875BA888(ctx, base);
}

// SessionWorld_RequestWriteStatsAllValid: only InGameHUD_GoShowSummary calls it.
REX_EXTERN(__imp__sub_875A6770);
REX_HOOK_RAW(sub_875A6770) {
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
  const uint64_t lr = ctx.lr;
  t_stats_requested = false;
  __imp__sub_87599B80(ctx, base);
  if (t_stats_requested) return;

  REXLOG_INFO("[Score] no platform session at the summary; requesting the stats write");
  RequestStatsWrite(ctx, base, lr);
  ctx.r3.u64 = 0;
  ctx.lr = lr;
}

// FireCommunicator_SendCharSelGamerTag(this, joy_id, slot_id, track_no): a controller that has a configured name is
// treated as signed in to the console, so the label shows that name and not "Player N".
REX_EXTERN(__imp__sub_875BA650);
REX_HOOK_RAW(sub_875BA650) {
  if (!HasLocalDisplayName(ctx.r4.u32)) {
    __imp__sub_875BA650(ctx, base);
    return;
  }
  ForcedImport signed_in(kImportPlayerSignedIntoConsole);
  __imp__sub_875BA650(ctx, base);
}

// BasicWorld_CheckIfPlayerCanViewProfile(this, joy_id, player_id): only the Gamer Card actions ask. A local player
// has no Xbox Live privileges, and the card shown is the local one.
REX_EXTERN(__imp__sub_8757E9E0);
REX_HOOK_RAW(sub_8757E9E0) {
  if (IsLocalGamerCardUser(ctx.r4.u32)) {
    ctx.r3.u64 = kPltSucceed;
    return;
  }
  __imp__sub_8757E9E0(ctx, base);
}

// Engine import table lookup used by every compiled script call.
REX_HOOK_RAW(sub_87520018) {
  if (ImportForced(ctx.r3.u32)) {
    ctx.r3.u64 = kReturnsTrue;
    return;
  }
  __imp__sub_87520018(ctx, base);
}
