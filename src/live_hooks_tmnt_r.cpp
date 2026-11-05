#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "live_scope.h"
#include "online/party_report.h"

using namespace tmnt;
using namespace tmnt::live;

namespace {

// A function in TMNT_r that nothing calls and that only returns true.
constexpr uint32_t kReturnsTrue = 0x8761B470;
constexpr int kMaxLoggedOverrides = 8;
constexpr uint32_t kWorldScriptOffset = 7216;

thread_local bool t_stats_requested = false;

// CharSelection's script variables: *(this + 8) is their block, whose layout CSL_InitializePersistentProfiles shows.
constexpr uint32_t kCharSelVars = 8;
constexpr uint32_t kCharSelChoices = 88;
constexpr uint32_t kCharSelConfirmed = 104;
constexpr uint32_t kCharSelStations = 136;
constexpr uint32_t kImportLocalStation = 1143;  // ONL_GetLocalStationId

online::PartySeats ReadPartySeats(const uint8_t* base, uint32_t script) {
  online::PartySeats seats;
  const uint32_t vars = LoadBE32(base + GuestHostOffset(script), script + kCharSelVars);
  const uint8_t* block = base + GuestHostOffset(vars);
  for (uint32_t i = 0; i < seats.size(); ++i) {
    seats[i].choice = int32_t(LoadBE32(block, vars + kCharSelChoices + 4 * i));
    seats[i].confirmed = int32_t(LoadBE32(block, vars + kCharSelConfirmed + 4 * i));
    seats[i].station = int32_t(LoadBE32(block, vars + kCharSelStations + 4 * i));
  }
  return seats;
}

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

REX_EXTERN(__imp__sub_87520018);

// After the original, so only registers the caller already expects to lose are touched.
void PublishPartySnapshot(PPCContext& ctx, uint8_t* base, const online::PartySeats& seats) {
  if (online::BookedSeats(seats) < online::kMinPartyReported) return;
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = kImportLocalStation;
  __imp__sub_87520018(ctx, base);
  PPCFunc* local_station = rex::runtime::ResolveIndirectFunction(ctx.r3.u32);
  if (!local_station) return;
  local_station(ctx, base);
  const int32_t station = ctx.r3.s32;
  ctx.lr = lr;
  const std::optional<online::PartySnapshot> snapshot = online::BuildPartySnapshot(seats, station);
  if (!snapshot) return;
  static std::atomic<uint32_t> sequence{0};
  rex::cvar::SetFlagByName("tmnt_online_party_snapshot", std::to_string(online::EncodePartySnapshot(*snapshot, ++sequence)));
}

// CSL_StartGame runs once on every machine when the party is final, so the party's turtles are read here and never
// written.
REX_EXTERN(__imp__sub_875E8A10);
REX_HOOK_RAW(sub_875E8A10) {
  const online::PartySeats seats = ReadPartySeats(base, ctx.r3.u32);
  __imp__sub_875E8A10(ctx, base);
  PublishPartySnapshot(ctx, base, seats);
}

// PLT_IsSignedIntoFullAccountOnline(controller in r5): the shared check behind every Live gate in the scripts.
REX_EXTERN(__imp__sub_876AE008);
REX_HOOK_RAW(sub_876AE008) {
  const int32_t controller = ctx.r5.s32;
  __imp__sub_876AE008(ctx, base);
  if (ctx.r3.u32 || !LocalProfileAccepted(controller)) return;
  ctx.r3.u64 = 1;
  LogLocalProfileAccepted(controller);
}

// SessionManager_CheckIfCanPlayOnline(this, controller, retry): the ONLINE row's gate. The controller that pressed it owns
// the LIVE profile the gate is about to read; the gate itself answers from the real sign-in state.
REX_EXTERN(__imp__sub_875D4DF0);
REX_HOOK_RAW(sub_875D4DF0) {
  const int32_t controller = ctx.r4.s32;
  if (IsLocalController(controller)) rex::cvar::SetFlagByName("tmnt_online_user", std::to_string(controller));
  __imp__sub_875D4DF0(ctx, base);
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

// FireCommunicator_GetMaxPlayers(this): the slots the title configured when it created the session. A guest never
// creates one, so its value is never written; the joined session's slots stand in.
REX_EXTERN(__imp__sub_875CCEC8);
REX_HOOK_RAW(sub_875CCEC8) {
  __imp__sub_875CCEC8(ctx, base);
  const std::string joined = rex::cvar::GetFlagByName("tmnt_online_joined_slots");
  const int slots = joined.empty() ? 0 : std::atoi(joined.c_str());
  if (slots > 0) ctx.r3.u64 = uint32_t(slots);
}

constexpr uint32_t kMenuTextPackage = 968;
constexpr char kXboxLiveLabel[] = "XBOX LIVE";
constexpr char kOnlineLabel[] = "ONLINE";

// A menu entry that names the Xbox LIVE service stands for this port's online service.
void RenameXboxLiveLabel(uint8_t* base, uint32_t strings) {
  const uint32_t pointer = LoadBE32(base, strings);
  if (pointer < 0x10000 || pointer >= 0xA0000000) return;
  if (std::memcmp(GuestHost(base, pointer), kXboxLiveLabel, sizeof kXboxLiveLabel) != 0) return;
  static const uint32_t label = [base] {
    const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof kOnlineLabel);
    if (address) std::memcpy(GuestHost(base, address), kOnlineLabel, sizeof kOnlineLabel);
    return address;
  }();
  if (label) StoreBE32(base, strings, label);
}

// FireCommunicator_SetPkg(this, id, int values, string pointers): every UI event the game hands to the FIRE movie.
REX_EXTERN(__imp__sub_875ADBC0);
REX_HOOK_RAW(sub_875ADBC0) {
  const uint32_t id = ctx.r4.u32;
  const uint32_t strings = ctx.r6.u32;
  if (id == kMenuTextPackage && strings) RenameXboxLiveLabel(base, strings);
  __imp__sub_875ADBC0(ctx, base);
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

// Engine import table lookup used by every compiled script call.
REX_HOOK_RAW(sub_87520018) {
  if (ImportForced(ctx.r3.u32)) {
    ctx.r3.u64 = kReturnsTrue;
    return;
  }
  __imp__sub_87520018(ctx, base);
}
