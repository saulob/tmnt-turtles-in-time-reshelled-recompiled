// Receiving a friend's invite: the accepted invite is handed to the title's own XN_LIVE_INVITE_ACCEPTED path, which
// reads XInviteGetAcceptedInfo, looks the session up with XSessionSearchByID and joins it with XSessionCreate.
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "invite_ui.h"
#include "online_runtime.h"
#include "player_match.h"
#include "player_match_net.h"

namespace {

using tmnt::GuestHost;
using tmnt::LoadBE32;
using tmnt::StoreBE32;

uint64_t SessionIdValue(const std::string& session) {
  const std::vector<uint8_t> bytes = tmnt::SessionIdBytes(session);
  uint64_t value = 0;
  for (const uint8_t byte : bytes) value = value << 8 | byte;
  return value;
}

std::optional<tmnt::online::AcceptedInvite> CurrentInvite() {
  if (!tmnt::OnlineRunning()) return std::nullopt;
  return tmnt::Online().AcceptedInviteInfo();
}

uint64_t LiveXuid() {
  if (!tmnt::OnlineRunning()) return 0;
  const tmnt::online::OnlineProfile profile = tmnt::Online().Profile();
  if (!tmnt::LiveAvailable(true, profile.signed_in) || profile.friend_code.empty()) return 0;
  return tmnt::VirtualXuid(profile.friend_code);
}

// (user, XINVITE_INFO*): the information the title's XN_LIVE_INVITE_ACCEPTED handler joins.
void InviteAcceptedInfo(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const std::optional<tmnt::online::AcceptedInvite> invite = user == tmnt::OnlineUser() && out ? CurrentInvite() : std::nullopt;
  const uint64_t xuid = invite ? LiveXuid() : 0;
  REXLOG_INFO("DEBUG_SAULO invite info read user={} out={:08X} answered={}", user, out, invite && xuid);
  if (!invite || !xuid) {
    original(ctx, base);
    return;
  }
  tmnt::WriteInviteInfo(GuestHost(base, out), xuid, tmnt::VirtualXuid(invite->host), invite->session);
  ctx.r3.u64 = 0;
}

// (XNKID by value, user, DWORD* result bytes, result header, overlapped): the invited session is answered from the
// invite; anything else goes to the title's own call.
void SessionSearchById(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint64_t id = ctx.r3.u64;
  const uint32_t size_ptr = ctx.r5.u32;
  const uint32_t header = ctx.r6.u32;
  const uint32_t overlapped = ctx.r7.u32;
  const std::optional<tmnt::online::AcceptedInvite> invite = size_ptr ? CurrentInvite() : std::nullopt;
  if (!invite || id != SessionIdValue(invite->session)) {
    original(ctx, base);
    return;
  }
  const uint32_t capacity = LoadBE32(GuestHost(base, size_ptr), 0);
  const bool first = capacity < tmnt::kSearchByIdBytes || !header;
  REXLOG_INFO("DEBUG_SAULO XSessionSearchByID session={} size={} call={} overlapped={:08X}", invite->session, capacity,
              first ? "first" : "second", overlapped);
  constexpr uint32_t kInsufficientBuffer = 122;
  if (first) {
    StoreBE32(GuestHost(base, size_ptr), 0, tmnt::kSearchByIdBytes);
    ctx.r3.u64 = kInsufficientBuffer;
    return;
  }
  tmnt::online::PlayerMatchAd ad;
  ad.session = invite->session;
  ad.player = invite->host;
  ad.name = invite->host_name;
  ad.slots = invite->slots;
  tmnt::WriteSearchByIdResult(GuestHost(base, header), header, capacity, ad);
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = tmnt::kIoPending;
  } else {
    ctx.r3.u64 = 0;
  }
}

// XamShowFriendsUI(user): the lobby's Invite Friends opens the Xbox Guide, which this port does not have. The friend
// list of the online service opens instead.
void ShowFriendsUi(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t user = ctx.r3.u32;
  const bool live = user == tmnt::OnlineUser() && LiveXuid() != 0;
  REXLOG_INFO("DEBUG_SAULO invite friend list requested user={} live={}", user, live);
  if (live && tmnt::OpenInvitePicker()) {
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

}  // namespace

namespace tmnt {

void DeliverAcceptedInvite(const online::AcceptedInvite& invite) {
  REXLOG_INFO("DEBUG_SAULO invite delivered generation={} session={} host={}", invite.generation, invite.session,
              invite.host);
  if (auto* kernel = REX_KERNEL_STATE()) {
    kernel->BroadcastNotification(kInviteAcceptedNotification, OnlineUser());
  } else {
    REXLOG_INFO("DEBUG_SAULO invite notification deferred: no kernel state yet");
  }
}

}  // namespace tmnt

#define TMNT_INVITE_HOOK(address, handler)     \
  REX_EXTERN(__imp__sub_##address);            \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

TMNT_INVITE_HOOK(823C76A8, InviteAcceptedInfo)
TMNT_INVITE_HOOK(8243CB30, SessionSearchById)
TMNT_INVITE_HOOK(826EDB40, ShowFriendsUi)
