// The title's own Live wrappers while Online play is on: a title-local LIVE profile for the online player, and the
// XSession calls that create, find, join and leave a match, answered from the online service.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <functional>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_memory.h"
#include "online_bridge.h"
#include "online_runtime.h"
#include "player_match.h"
#include "player_match_net.h"
#include "qos_placeholder.h"
#include "search_ticket.h"

// Slots of the session this console joined as a guest; the title only configures them when it creates a session, and
// TMNT_r reads this to show them (live_hooks_tmnt_r.cpp).
REXCVAR_DEFINE_INT32(tmnt_online_joined_slots, 0, "TMNT", "Player slots of the online session joined as a guest; 0: none");
// Set by TMNT_r from the ONLINE row's gate.
REXCVAR_DEFINE_INT32(tmnt_online_user, 0, "TMNT", "Controller that chose ONLINE and owns the LIVE profile");

namespace {

using tmnt::GuestHost;
using tmnt::LoadBE32;
using tmnt::StoreBE32;
using tmnt::StoreBE64;

uint32_t CurrentOnlineUser() {
  const uint32_t user = uint32_t(REXCVAR_GET(tmnt_online_user));
  return tmnt::LocalUser(user) ? user : 0;
}

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
    REXLOG_INFO("online runtime LIVE state active xuid={:016X}", live.xuid);
  }
  return live;
}

constexpr std::chrono::seconds kSearchWaitLimit(7);
// Below the 15 s after which the title's join script gives up on the call.
constexpr std::chrono::seconds kJoinWaitLimit(7);
// A cached answer still completes after a few script ticks, so the title's wait overlay is shown before its result.
constexpr std::chrono::milliseconds kSearchLatencyFloor(250);

// What the game believes its XSession holds: the members it joined (it calls XSessionJoinRemote on every console for
// every player, its own included), the slots it asked for and the state it moved the session to.
std::mutex g_session_mutex;
tmnt::SessionDetails g_session;
uint32_t g_members_address = 0;

void PublishJoinedSlots(uint32_t slots) {
  rex::cvar::SetFlagByName("tmnt_online_joined_slots", std::to_string(slots >= 2 && slots <= tmnt::online::kMaxParty ? slots : 0));
}

void ResetSession() {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  g_session = {};
  PublishJoinedSlots(0);
}
// What the title's XSessionSearchEx wrapper requires per requested result.
constexpr uint32_t kSearchBytesPerResult = 1326;

void SigninState(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t online_user = CurrentOnlineUser();
  if (tmnt::LiveProfileUser(user, online_user, CurrentLive().available)) {
    ctx.r3.u64 = tmnt::kSignedInToLive;
  } else {
    original(ctx, base);
  }
}

void CheckPrivilege(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t result = ctx.r5.u32;
  if (result && tmnt::LivePrivilegeUser(user, CurrentOnlineUser(), CurrentLive().available)) {
    StoreBE32(GuestHost(base, result), 0, 1);
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

void SigninInfo(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t info = ctx.r5.u32;
  original(ctx, base);
  const LiveProfile live = user == CurrentOnlineUser() && info ? CurrentLive() : LiveProfile{};
  if (!live.available) return;
  tmnt::WriteSigninInfo(GuestHost(base, info), live.xuid, live.name);
  ctx.r3.u64 = 0;
}

void GetXuid(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const LiveProfile live = user == CurrentOnlineUser() && out ? CurrentLive() : LiveProfile{};
  if (!live.available) {
    original(ctx, base);
    return;
  }
  StoreBE64(GuestHost(base, out), 0, live.xuid);
  ctx.r3.u64 = 0;
}

// Also the name the game's peer layer gives this console's station.
void GetName(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const LiveProfile live = user == CurrentOnlineUser() && buffer && size ? CurrentLive() : LiveProfile{};
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
  tmnt::WriteXnAddr(GuestHost(base, xnaddr), live.xuid, tmnt::ActiveSyntheticAddress(live.xuid));
  constexpr uint32_t kXnAddrOnline = 0x80;
  ctx.r3.u64 = ctx.r3.u32 | kXnAddrOnline;
}

// XUserMuteListQuery(user, XUID, BOOL*): XLIVEBASE 0x0005800E, behind the scripts' PLT_IsPlayerMuted. The runtime has no
// handler for it; this port has no voice chat and no mute list, so nobody is on it.
void MuteListQuery(PPCContext& ctx, uint8_t* base, PPCFunc*) {
  if (ctx.r5.u32) StoreBE32(GuestHost(base, ctx.r5.u32), 0, 0);
  ctx.r3.u64 = 0;
}

void EndOnlineMatch(const char* reason) {
  tmnt::Online().CancelPlayerMatch();
  tmnt::Online().LeaveFinishedMatch();
  ResetSession();
  REXLOG_INFO("player_match session left reason={}", reason);
}

// The join's overlapped stays pending until the service has answered, so a refusal reaches the title's own failure path
// instead of its wait for a host that will never answer. The runtime already completed it; nothing has looked at it yet.
void CompleteJoinWhenAnswered(bool started, uint32_t overlapped) {
  const auto limit = std::chrono::steady_clock::now() + kJoinWaitLimit;
  auto run = [=]() -> rex::X_RESULT {
    while (started && tmnt::Online().JoinAnswer().pending && std::chrono::steady_clock::now() < limit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const tmnt::online::OnlineService::PlayerMatchJoinAnswer answer = tmnt::Online().JoinAnswer();
    const uint32_t result = tmnt::JoinRefusalResult(answer.pending ? std::string("link_lost") : answer.refused);
    if (!result) return 0;
    REXLOG_INFO("player_match join failed result={:08X} code={}", result, answer.pending ? "timeout" : answer.refused);
    EndOnlineMatch("join_refused");
    return result;
  };
  REX_KERNEL_STATE()->CompleteOverlappedDeferred(std::function<rex::X_RESULT()>(std::move(run)), overlapped);
}

// Host: advertised to online friends. Join (a search result's session info): the service pairs the two consoles.
void SessionCreate(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t flags = ctx.r3.u32;
  const uint32_t info = ctx.r8.u32;
  const bool live = CurrentLive().available;
  std::string join_session;
  if (live && !(flags & tmnt::kSessionCreateHost) && info) {
    join_session = tmnt::SessionIdText(GuestHost(base, info));
  }
  const uint32_t public_slots = ctx.r5.u32;
  const uint32_t private_slots = ctx.r6.u32;
  const uint32_t overlapped = ctx.r9.u32;
  original(ctx, base);
  if (!live) return;
  if (flags & tmnt::kSessionCreateHost) {
    {
      std::lock_guard<std::mutex> lock(g_session_mutex);
      g_session = {};
      g_session.max_public = public_slots;
      g_session.max_private = private_slots;
      g_session.host_xuid = CurrentLive().xuid;
      PublishJoinedSlots(0);
    }
    if (info) tmnt::WriteXnAddr(GuestHost(base, info) + tmnt::kXnkidBytes, CurrentLive().xuid);
    tmnt::Online().HostPlayerMatch({}, int(std::clamp<uint32_t>(public_slots + private_slots, 2, tmnt::online::kMaxParty)));
    return;
  }
  if (!join_session.empty()) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session = {};
    g_session.session = join_session;
    tmnt::SessionSlots slots = tmnt::SearchResultSlots(GuestHost(base, info));
    for (const tmnt::online::PlayerMatchAd& ad : tmnt::Online().PlayerMatchResults()) {
      if (ad.session != join_session) continue;
      g_session.host_xuid = tmnt::VirtualXuid(ad.player);
      slots = {uint32_t(ad.slots), 0};
    }
    g_session.max_public = slots.max_public;
    g_session.max_private = slots.max_private;
    PublishJoinedSlots(slots.Total());
  }
  if (join_session.empty()) return;
  const bool started = tmnt::Online().JoinPlayerMatch(join_session);
  if (!started) REXLOG_INFO("player_match join refused session={}", join_session);
  if (overlapped && ctx.r3.u32 == tmnt::kIoPending) CompleteJoinWhenAnswered(started, overlapped);
}

// The guest's search stays pending until the service has answered it, so an empty cache is never taken for "no match".
tmnt::SearchTickets g_search_tickets;

void CompleteSearchWhenAnswered(uint8_t* base, uint32_t epoch, uint32_t header, uint32_t size_ptr,
                                uint32_t max_results, uint32_t overlapped) {
  const uint32_t ticket = g_search_tickets.Begin();
  const auto started = std::chrono::steady_clock::now();
  auto run = [=]() -> rex::X_RESULT {
    const auto limit = started + kSearchWaitLimit;
    while (tmnt::Online().PlayerMatchSearchPending(epoch) && g_search_tickets.Owns(ticket) &&
           std::chrono::steady_clock::now() < limit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_until(started + kSearchLatencyFloor);
    if (!g_search_tickets.Complete(ticket)) return 0;
    const uint32_t capacity = LoadBE32(GuestHost(base, size_ptr), 0);
    tmnt::WriteSearchResults(GuestHost(base, header), header, capacity, max_results,
                             tmnt::Online().PlayerMatchResults(epoch));
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
  tmnt::WriteSearchResults(GuestHost(base, header), header, capacity, max_results,
                           tmnt::Online().PlayerMatchResults());
}

// ReXGlue fails the XGI leave message (0x000B0013), so the game would never see a player leave. Membership lives in
// the online service, so the leave only has to complete.
void SessionLeaveRemote(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t overlapped = ctx.r6.u32;
  if (!CurrentLive().available) {
    original(ctx, base);
    return;
  }
  if (ctx.r5.u32 && ctx.r4.u32 && ctx.r4.u32 <= 8) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    for (uint32_t i = 0; i < ctx.r4.u32; ++i) {
      const uint64_t xuid = tmnt::LoadBE64(GuestHost(base, ctx.r5.u32), i * 8);
      g_session.members.erase(std::remove_if(g_session.members.begin(), g_session.members.end(),
                                             [xuid](const tmnt::SessionMember& m) { return m.xuid == xuid; }),
                              g_session.members.end());
    }
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
        g_session.members.push_back({xuid, xuid == live.xuid ? CurrentOnlineUser() : tmnt::kRemoteUserIndex});
      }
    }
  }
  original(ctx, base);
}

void SessionStateChange(PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t state) {
  if (CurrentLive().available) {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    g_session.state = state;
  }
  original(ctx, base);
}

void SessionStart(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  SessionStateChange(ctx, base, original, tmnt::kSessionStateInGame);
  if (CurrentLive().available) tmnt::Online().NoteGameStart();
}

void SessionEnd(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  SessionStateChange(ctx, base, original, tmnt::kSessionStateReporting);
}

void SessionGetDetails(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const LiveProfile live = CurrentLive();
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
  if (details.members.empty()) details.members.push_back({live.xuid, CurrentOnlineUser()});
  if (details.session.empty()) {
    if (const auto party = tmnt::Online().party()) details.session = party->session.empty() ? party->match.substr(0, 16) : party->session;
  }
  tmnt::WriteSessionDetails(GuestHost(base, buffer), GuestHost(base, g_members_address), g_members_address, details);
  if (details.host_xuid) tmnt::WriteXnAddr(GuestHost(base, buffer) + tmnt::kDetailsHostOffset, details.host_xuid);
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = tmnt::kIoPending;
  } else {
    ctx.r3.u64 = 0;
  }
}

void SessionDelete(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  original(ctx, base);
  ResetSession();
  if (!CurrentLive().available) return;
  EndOnlineMatch("session_deleted");
}

}  // namespace

// The title's search object keeps an inline zeroed XNQOS placeholder at object+16 and releases it on every reset; nothing
// was allocated, and the stubbed QoS lookup never replaces it, so a real release only fails inside BaseHeap.
uint32_t tmnt::OnlineUser() { return CurrentOnlineUser(); }

void QosRelease(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kSearchObjectGlobal = 0x82BAD418;
  const uint32_t object = tmnt::LoadBE32(GuestHost(base, kSearchObjectGlobal), 0);
  if (tmnt::IsInlineQosPlaceholder(ctx.r3.u32, object)) {
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

#define TMNT_LIVE_HOOK(address, handler)       \
  REX_EXTERN(__imp__sub_##address);            \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

TMNT_LIVE_HOOK(826EDD50, SigninState)
TMNT_LIVE_HOOK(826EDD60, CheckPrivilege)
TMNT_LIVE_HOOK(826EE0C8, SigninInfo)
TMNT_LIVE_HOOK(826EE078, GetXuid)
TMNT_LIVE_HOOK(826EDD48, GetName)
TMNT_LIVE_HOOK(823CB4E0, TitleXnAddr)
TMNT_LIVE_HOOK(823C7F20, MuteListQuery)
TMNT_LIVE_HOOK(8243C480, SessionCreate)
TMNT_LIVE_HOOK(8243CA48, SessionSearchEx)
TMNT_LIVE_HOOK(8243C850, SessionLeaveRemote)
TMNT_LIVE_HOOK(8243C6F0, SessionDelete)
TMNT_LIVE_HOOK(8243C7A0, SessionJoinRemote)
TMNT_LIVE_HOOK(823CB4D0, QosRelease)
TMNT_LIVE_HOOK(8243C8F8, SessionStart)
TMNT_LIVE_HOOK(8243C998, SessionEnd)
TMNT_LIVE_HOOK(8243CD78, SessionGetDetails)
