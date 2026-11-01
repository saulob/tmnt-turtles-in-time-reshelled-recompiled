// The title's own Live wrappers while Online play is on: a title-local LIVE profile for the online player, and the
// XSession calls that create, find, join and leave a match, answered from the online service.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <functional>
#include <string>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "frame_trace.h"
#include "guest_memory.h"
#include "online_runtime.h"
#include "player_match.h"
#include "player_match_net.h"
#include "qos_placeholder.h"
#include "search_ticket.h"

namespace {

using tmnt::GuestHost;
using tmnt::LoadBE32;
using tmnt::StoreBE32;
using tmnt::StoreBE64;

struct LiveProfile {
  bool available = false;
  uint64_t xuid = 0;
  std::string name;
};

LiveProfile CurrentLive() {
  LiveProfile live;
  if (!tmnt::OnlineRunning()) return live;
  const tmnt::online::OnlineProfile profile = tmnt::Online().Profile();
  live.available = tmnt::LiveAvailable(true, profile.signed_in) && !profile.friend_code.empty();
  if (!live.available) return live;
  live.xuid = tmnt::VirtualXuid(profile.friend_code);
  live.name = profile.name;
  static uint64_t logged_xuid = 0;
  if (logged_xuid != live.xuid) {
    logged_xuid = live.xuid;
    REXLOG_INFO("DEBUG_SAULO online runtime LIVE state active xuid={:016X}", live.xuid);
  }
  return live;
}

constexpr std::chrono::seconds kSearchWaitLimit(7);
// A cached answer still completes after a few script ticks, so the title's wait overlay is shown before its result.
constexpr std::chrono::milliseconds kSearchLatencyFloor(250);

// What the game believes its XSession holds: the members it joined (it calls XSessionJoinRemote on every console for
// every player, its own included), the slots it asked for and the state it moved the session to.
std::mutex g_session_mutex;
tmnt::SessionDetails g_session;
uint32_t g_members_address = 0;

void ResetSession() {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  g_session = {};
}
// What the title's XSessionSearchEx wrapper requires per requested result.
constexpr uint32_t kSearchBytesPerResult = 1326;

// One line per distinct value, so per-frame callers do not flood the log.
void LogOnChange(const char* what, uint32_t value) {
  struct Seen {
    const char* what;
    uint32_t value;
  };
  static Seen seen[16] = {};
  for (Seen& s : seen) {
    if (s.what == what) {
      if (s.value == value) return;
      s.value = value;
      REXLOG_INFO("DEBUG_SAULO {} value={}", what, value);
      return;
    }
    if (!s.what) {
      s = {what, value};
      REXLOG_INFO("DEBUG_SAULO {} value={}", what, value);
      return;
    }
  }
}

void SigninState(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  if (user == tmnt::kOnlineUser && CurrentLive().available) {
    ctx.r3.u64 = tmnt::kSignedInToLive;
  } else {
    original(ctx, base);
  }
  if (user == tmnt::kOnlineUser) LogOnChange("live signin state", ctx.r3.u32);
}

void CheckPrivilege(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t privilege = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32;
  constexpr uint32_t kAnyUser = 0xFF;
  if ((user == tmnt::kOnlineUser || user == kAnyUser) && result && CurrentLive().available) {
    StoreBE32(GuestHost(base, result), 0, 1);
    ctx.r3.u64 = 0;
    LogOnChange("live privilege check granted", privilege);
    return;
  }
  original(ctx, base);
}

void SigninInfo(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t info = ctx.r5.u32;
  original(ctx, base);
  const LiveProfile live = user == tmnt::kOnlineUser && info ? CurrentLive() : LiveProfile{};
  if (!live.available) return;
  uint8_t* out = GuestHost(base, info);
  StoreBE64(out, 0, live.xuid);
  StoreBE32(out, 8, tmnt::kSigninInfoOnlineEnabled);
  StoreBE32(out, 12, tmnt::kSignedInToLive);
  std::memset(out + 24, 0, 16);
  std::memcpy(out + 24, live.name.data(), std::min<size_t>(live.name.size(), 15));
  ctx.r3.u64 = 0;
  LogOnChange("live signin info", tmnt::kSignedInToLive);
}

void GetXuid(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const LiveProfile live = user == tmnt::kOnlineUser && out ? CurrentLive() : LiveProfile{};
  if (!live.available) {
    original(ctx, base);
    return;
  }
  StoreBE64(GuestHost(base, out), 0, live.xuid);
  ctx.r3.u64 = 0;
  LogOnChange("live xuid virtual", uint32_t(live.xuid));
}

// Also the name the game's peer layer gives this console's station.
void GetName(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const LiveProfile live = user == tmnt::kOnlineUser && buffer && size ? CurrentLive() : LiveProfile{};
  if (!live.available || live.name.empty()) {
    original(ctx, base);
    return;
  }
  const uint32_t capacity = std::min<uint32_t>(size, 16);
  std::memset(GuestHost(base, buffer), 0, capacity);
  std::memcpy(GuestHost(base, buffer), live.name.data(), std::min<size_t>(live.name.size(), capacity - 1));
  ctx.r3.u64 = 0;
}

void TitleXnAddr(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t xnaddr = ctx.r3.u32;
  original(ctx, base);
  const LiveProfile live = CurrentLive();
  if (!live.available || !xnaddr) return;
  tmnt::WriteXnAddr(GuestHost(base, xnaddr), live.xuid);
  constexpr uint32_t kXnAddrOnline = 0x80;
  ctx.r3.u64 = ctx.r3.u32 | kXnAddrOnline;
  LogOnChange("live xnaddr flags", ctx.r3.u32);
}

// The game's own way out of its session ends the game session; the party and its connections stay.
void EndOnlineMatch(const char* reason) {
  tmnt::Online().CancelPlayerMatch();
  ResetSession();
  REXLOG_INFO("DEBUG_SAULO player_match session left reason={}", reason);
}

// Host: advertised to online friends. Join (a search result's session info): the service pairs the two consoles.
void SessionCreate(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t flags = ctx.r3.u32;
  const uint32_t info = ctx.r8.u32;
  REXLOG_INFO("DEBUG_SAULO XSessionCreate flags={:08X} user={} public={} private={}", flags, ctx.r4.u32, ctx.r5.u32,
              ctx.r6.u32);
  const bool live = CurrentLive().available;
  std::string join_session;
  if (live && !(flags & tmnt::kSessionCreateHost) && info) {
    join_session = tmnt::SessionIdText(GuestHost(base, info));
  }
  const uint32_t public_slots = ctx.r5.u32;
  const uint32_t private_slots = ctx.r6.u32;
  original(ctx, base);
  if (!live) return;
  if (flags & tmnt::kSessionCreateHost) {
    {
      std::lock_guard<std::mutex> lock(g_session_mutex);
      g_session = {};
      g_session.max_public = public_slots;
      g_session.max_private = private_slots;
      g_session.host_xuid = CurrentLive().xuid;
    }
    if (info) tmnt::WriteXnAddr(GuestHost(base, info) + tmnt::kXnkidBytes, CurrentLive().xuid);
    tmnt::Online().HostPlayerMatch({}, int(std::clamp<uint32_t>(public_slots + private_slots, 2, tmnt::online::kMaxParty)));
    return;
  }
  if (!join_session.empty()) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session = {};
    g_session.session = join_session;
    g_session.max_public = LoadBE32(GuestHost(base, info), tmnt::kSessionInfoBytes) +
                           LoadBE32(GuestHost(base, info), tmnt::kSessionInfoBytes + 8);
    g_session.max_private = LoadBE32(GuestHost(base, info), tmnt::kSessionInfoBytes + 4) +
                            LoadBE32(GuestHost(base, info), tmnt::kSessionInfoBytes + 12);
    for (const tmnt::online::PlayerMatchAd& ad : tmnt::Online().PlayerMatchResults()) {
      if (ad.session == join_session) g_session.host_xuid = tmnt::VirtualXuid(ad.player);
    }
  }
  if (!join_session.empty() && !tmnt::Online().JoinPlayerMatch(join_session)) {
    REXLOG_INFO("DEBUG_SAULO player_match join refused session={}", join_session);
  }
}

// The guest's search stays pending until the service has answered it, so an empty cache is never taken for "no match".
std::atomic<bool> g_search_active{false};
tmnt::SearchTickets g_search_tickets;
constexpr uint32_t kGameSlotFlag = 0x82BD17D0;  // claim flag of the title's single overlapped slot
std::atomic<int> g_pool_frees_logged{0};

void CompleteSearchWhenAnswered(uint8_t* base, uint32_t epoch, uint32_t header, uint32_t size_ptr,
                                uint32_t max_results, uint32_t overlapped) {
  const uint32_t ticket = g_search_tickets.Begin();
  g_search_active = true;
  g_pool_frees_logged = 0;
  REXLOG_INFO("DEBUG_SAULO XSessionSearchEx ticket={} epoch={} header={:08X} size_ptr={:08X} overlapped={:08X} max={} slot={}",
              ticket, epoch, header, size_ptr, overlapped, max_results, LoadBE32(GuestHost(base, kGameSlotFlag), 0));
  const auto started = std::chrono::steady_clock::now();
  auto run = [=]() -> rex::X_RESULT {
    const auto limit = started + kSearchWaitLimit;
    while (tmnt::Online().PlayerMatchSearchPending(epoch) && g_search_tickets.Owns(ticket) &&
           std::chrono::steady_clock::now() < limit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_until(started + kSearchLatencyFloor);
    if (!g_search_tickets.Complete(ticket)) {
      REXLOG_INFO("DEBUG_SAULO player_match search ticket={} stale, result discarded", ticket);
      return 0;
    }
    const uint32_t capacity = LoadBE32(GuestHost(base, size_ptr), 0);
    const uint32_t written = tmnt::WriteSearchResults(GuestHost(base, header), header, capacity, max_results,
                                                      tmnt::Online().PlayerMatchResults(epoch));
    REXLOG_INFO("DEBUG_SAULO player_match search ticket={} results={} deferred=true slot={}", ticket, written,
                LoadBE32(GuestHost(base, kGameSlotFlag), 0));
    g_search_active = false;
    return 0;
  };
  REX_KERNEL_STATE()->CompleteOverlappedDeferred(std::function<rex::X_RESULT()>(std::move(run)), overlapped);
}

void SessionSearchEx(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t max_results = ctx.r5.u32;
  const uint32_t stack = ctx.r1.u32;
  const uint32_t size_ptr = LoadBE32(base, stack + 84);
  const uint32_t header = LoadBE32(base, stack + 92);
  const uint32_t overlapped = LoadBE32(base, stack + 100);
  const bool live = CurrentLive().available;
  tmnt::online::OnlineService::PlayerMatchSearchWait search;
  if (live) search = tmnt::Online().AskPlayerMatchSearch();
  static uint32_t calls_for_epoch = 0;
  static uint32_t last_epoch = 0;
  calls_for_epoch = search.epoch == last_epoch ? calls_for_epoch + 1 : 1;
  last_epoch = search.epoch;
  REXLOG_INFO("DEBUG_SAULO SearchEx call lr={:08X} epoch={} nth={} header={:08X} overlapped={:08X} pending={} slot={}",
              uint32_t(ctx.lr), search.epoch, calls_for_epoch, header, overlapped, search.pending,
              LoadBE32(GuestHost(base, kGameSlotFlag), 0));
  if (live && header && size_ptr && overlapped && max_results &&
      LoadBE32(GuestHost(base, size_ptr), 0) >= tmnt::kSearchHeaderBytes + max_results * kSearchBytesPerResult) {
    CompleteSearchWhenAnswered(base, search.epoch, header, size_ptr, max_results, overlapped);
    ctx.r3.u64 = tmnt::kIoPending;
    return;
  }
  original(ctx, base);
  constexpr uint32_t kInsufficientBuffer = 122;
  if (!live || ctx.r3.u32 == kInsufficientBuffer || !header || !size_ptr) return;
  const uint32_t capacity = LoadBE32(GuestHost(base, size_ptr), 0);
  const uint32_t written = tmnt::WriteSearchResults(GuestHost(base, header), header, capacity, max_results,
                                                    tmnt::Online().PlayerMatchResults());
  LogOnChange("player_match search results", written);
}

void SessionCall(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* name) {
  if (CurrentLive().available) REXLOG_INFO("DEBUG_SAULO {} handle={:08X} arg={:08X}", name, ctx.r3.u32, ctx.r4.u32);
  original(ctx, base);
}

// ReXGlue fails the XGI leave message (0x000B0013), so the game would never see a player leave. Membership lives in
// the online service, so the leave only has to complete.
void SessionLeaveRemote(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t overlapped = ctx.r6.u32;
  if (!CurrentLive().available) {
    original(ctx, base);
    return;
  }
  REXLOG_INFO("DEBUG_SAULO XSessionLeaveRemote handle={:08X} users={} overlapped={:08X}", ctx.r3.u32, ctx.r4.u32,
              overlapped);
  if (ctx.r5.u32 && ctx.r4.u32 && ctx.r4.u32 <= 8) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    REXLOG_INFO("DEBUG_SAULO XSessionLeaveRemote session={} host={} local={:016X} state={} members_before={} lr={:08X}",
                g_session.session, g_session.host_xuid == CurrentLive().xuid ? "local" : "remote", CurrentLive().xuid,
                g_session.state, g_session.members.size(), uint32_t(ctx.lr));
    for (uint32_t i = 0; i < ctx.r4.u32; ++i) {
      const uint64_t xuid = tmnt::LoadBE64(GuestHost(base, ctx.r5.u32), i * 8);
      REXLOG_INFO("DEBUG_SAULO XSessionLeaveRemote xuid={:016X} own={}", xuid, xuid == CurrentLive().xuid);
      g_session.members.erase(std::remove_if(g_session.members.begin(), g_session.members.end(),
                                             [xuid](const tmnt::SessionMember& m) { return m.xuid == xuid; }),
                              g_session.members.end());
    }
    REXLOG_INFO("DEBUG_SAULO XSessionLeaveRemote members_after={}", g_session.members.size());
  }
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = tmnt::kIoPending;
  } else {
    ctx.r3.u64 = 0;
  }
}

void SessionJoinRemote(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t count = ctx.r4.u32;
  const uint32_t xuids = ctx.r5.u32;
  const LiveProfile live = CurrentLive();
  if (live.available && xuids && count && count <= 8) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t xuid = tmnt::LoadBE64(GuestHost(base, xuids), i * 8);
      const bool known = std::any_of(g_session.members.begin(), g_session.members.end(),
                                     [xuid](const tmnt::SessionMember& m) { return m.xuid == xuid; });
      if (!known) {
        g_session.members.push_back({xuid, xuid == live.xuid ? tmnt::kOnlineUser : tmnt::kRemoteUserIndex});
        REXLOG_INFO("DEBUG_SAULO XSessionJoinRemote member={} local={} members={}", g_session.members.size(),
                    xuid == live.xuid, g_session.members.size());
      }
    }
  }
  original(ctx, base);
}

void SessionStateChange(PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t state, const char* name) {
  if (CurrentLive().available) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session.state = state;
  }
  SessionCall(ctx, base, original, name);
}

void SessionStart(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  SessionStateChange(ctx, base, original, tmnt::kSessionStateInGame, "XSessionStart");
  if (CurrentLive().available) tmnt::Online().NoteGameStart();
}

void SessionEnd(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  SessionStateChange(ctx, base, original, tmnt::kSessionStateReporting, "XSessionEnd");
}

void SessionGetDetails(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const LiveProfile live = CurrentLive();
  const uint32_t handle = ctx.r3.u32;
  const uint32_t size_ptr = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t overlapped = ctx.r6.u32;
  if (!live.available || !buffer || !size_ptr) {
    original(ctx, base);
    return;
  }
  constexpr uint32_t kInsufficientBuffer = 122;
  const uint32_t size = LoadBE32(GuestHost(base, size_ptr), 0);
  if (size < tmnt::kDetailsBytes) {
    StoreBE32(GuestHost(base, size_ptr), 0, tmnt::kDetailsBytes);
    ctx.r3.u64 = kInsufficientBuffer;
    return;
  }
  if (!g_members_address) g_members_address = REX_KERNEL_MEMORY()->SystemHeapAlloc(tmnt::kMemberBytes * tmnt::online::kMaxParty);
  if (!g_members_address) {
    original(ctx, base);
    return;
  }
  tmnt::SessionDetails details;
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    details = g_session;
  }
  if (details.members.empty()) details.members.push_back({live.xuid, tmnt::kOnlineUser});
  if (details.session.empty()) {
    if (const auto party = tmnt::Online().party()) details.session = party->session.empty() ? party->match.substr(0, 16) : party->session;
  }
  tmnt::WriteSessionDetails(GuestHost(base, buffer), GuestHost(base, g_members_address), g_members_address, details);
  if (details.host_xuid) tmnt::WriteXnAddr(GuestHost(base, buffer) + tmnt::kDetailsHostOffset, details.host_xuid);
  LogOnChange("XSessionGetDetails members", uint32_t(details.members.size()));
  tmnt::ArmFrameTrace(400);
  REXLOG_INFO("DEBUG_SAULO XSessionGetDetails handle={:08X} buffer={:08X} size={} overlapped={:08X} members={} lr={:08X}", handle,
              buffer, size, overlapped, details.members.size(), uint32_t(ctx.lr));
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = tmnt::kIoPending;
  } else {
    ctx.r3.u64 = 0;
  }
}

void SessionDelete(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  REXLOG_INFO("DEBUG_SAULO XSessionDelete lr={:08X}", uint32_t(ctx.lr));
  SessionCall(ctx, base, original, "XSessionDelete");
  ResetSession();
  if (!CurrentLive().available) return;
  tmnt::Online().CancelPlayerMatch();
  EndOnlineMatch("session_deleted");
}

}  // namespace

// The title's search object keeps an inline zeroed XNQOS placeholder at object+16 and releases it on every reset; nothing
// was allocated, and the stubbed QoS lookup never replaces it, so a real release only fails inside BaseHeap.
std::string tmnt::SessionHeartbeat(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  return "session_state=" + std::to_string(g_session.state) + " session_members=" + std::to_string(g_session.members.size()) +
         " slot_flag=" + std::to_string(LoadBE32(GuestHost(base, kGameSlotFlag), 0));
}

void QosRelease(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kSearchObjectGlobal = 0x82BAD418;
  const uint32_t object = tmnt::LoadBE32(GuestHost(base, kSearchObjectGlobal), 0);
  if (tmnt::IsInlineQosPlaceholder(ctx.r3.u32, object)) {
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

void PoolFree(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g_search_active && g_pool_frees_logged < 24) {
    ++g_pool_frees_logged;
    REXLOG_INFO("DEBUG_SAULO search-time pool free ptr={:08X} pool={:08X} lr={:08X}", ctx.r4.u32, ctx.r3.u32, uint32_t(ctx.lr));
  }
  original(ctx, base);
}

#define TMNT_LIVE_HOOK(address, handler)       \
  REX_EXTERN(__imp__sub_##address);            \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }
#define TMNT_SESSION_LOG(address, name)        \
  REX_EXTERN(__imp__sub_##address);            \
  REX_HOOK_RAW(sub_##address) { SessionCall(ctx, base, __imp__sub_##address, name); }

TMNT_LIVE_HOOK(826EDD50, SigninState)
TMNT_LIVE_HOOK(826EDD60, CheckPrivilege)
TMNT_LIVE_HOOK(826EE0C8, SigninInfo)
TMNT_LIVE_HOOK(826EE078, GetXuid)
TMNT_LIVE_HOOK(826EDD48, GetName)
TMNT_LIVE_HOOK(823CB4E0, TitleXnAddr)
TMNT_LIVE_HOOK(8243C480, SessionCreate)
TMNT_LIVE_HOOK(8243CA48, SessionSearchEx)
TMNT_LIVE_HOOK(8243C850, SessionLeaveRemote)
TMNT_LIVE_HOOK(8243C6F0, SessionDelete)
TMNT_LIVE_HOOK(8243C7A0, SessionJoinRemote)
TMNT_LIVE_HOOK(824B4F28, PoolFree)
TMNT_LIVE_HOOK(823CB4D0, QosRelease)
TMNT_LIVE_HOOK(8243C8F8, SessionStart)
TMNT_LIVE_HOOK(8243C998, SessionEnd)
TMNT_LIVE_HOOK(8243CD78, SessionGetDetails)
TMNT_SESSION_LOG(8243C630, "XSessionModify")
