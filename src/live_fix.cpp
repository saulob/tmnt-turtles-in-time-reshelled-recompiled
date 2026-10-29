#include "live_fix.h"

#include <atomic>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"

namespace tmnt::live {
namespace {

struct Services {
  Services(std::filesystem::path file, LocalUser user) : stats(std::move(file)), board(stats, std::move(user)) {}

  LocalStats stats;
  LocalLeaderboard board;
  EnumeratorRegistry enumerators;
};

// Never freed: hooks may still run while the process exits.
std::atomic<Services*> g_services{nullptr};

Services* Active() { return g_services.load(std::memory_order_acquire); }

constexpr uint32_t kLeaderboardRegistryGlobal = 0x82BAD420;

bool CreateStatsEnumerator(PPCContext& ctx, uint8_t* base, StatsQuery::Kind kind) {
  Services* services = Active();
  if (!services) return false;

  const uint64_t pivot = kind == StatsQuery::Kind::kRanked ? ctx.r4.u32 : ctx.r4.u64;
  ctx.r3.u64 = CreateEnumerator(base, services->enumerators, kind, pivot, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                                ctx.r8.u32, ctx.r9.u32);
  return true;
}

uint32_t CompletionStatus(const uint8_t* base, uint32_t status, uint32_t overlapped) {
  return status == kErrorIoPending && overlapped ? LoadBE32(base, overlapped) : status;
}

const char* KindName(StatsQuery::Kind kind) {
  switch (kind) {
    case StatsQuery::Kind::kRanked:
      return "rank";
    case StatsQuery::Kind::kByXuid:
      return "xuid";
    case StatsQuery::Kind::kFriends:
      return "friends";
  }
  return "?";
}

void LogQuery(const char* kind, const std::vector<StatsView>& views, uint64_t pivot, uint32_t status) {
  size_t rows = 0;
  for (const StatsView& view : views) rows += view.rows.size();
  REXLOG_INFO("[DEBUG_SAULO][Leaderboard] query kind={} view={} pivot={} -> status={:#x} rows={}", kind,
              views.empty() ? 0 : views.front().id, pivot, status, rows);
}

void RecordWrite(Services& services, uint64_t xuid, const ViewWrite& write) {
  const RecordResult result = services.stats.Record(xuid, write.view, write.stats);
  REXLOG_INFO("[DEBUG_SAULO][Stats] local score view={} rating={} {}", write.view, write.stats.rating,
              result == RecordResult::kSaved       ? "saved"
              : result == RecordResult::kUnchanged ? "unchanged"
                                                   : "not saved");
}

void RecordLocalWrites(Services& services, uint64_t xuid, const std::vector<ViewWrite>& writes) {
  for (const ViewWrite& write : writes) {
    if (xuid != services.board.user().xuid) {
      REXLOG_WARN("[DEBUG_SAULO][Stats] write for xuid {:016X}, local is {:016X}", xuid, services.board.user().xuid);
      continue;
    }
    if (write.stats.rating > 0) RecordWrite(services, xuid, write);
  }
}

}  // namespace

void Install(InstallParams params) {
  auto* services = new Services(std::move(params.stats_file), std::move(params.user));
  const LoadResult loaded = services->stats.Load();
  REXLOG_INFO("[DEBUG_SAULO][Stats] Local stats file {}",
              loaded == LoadResult::kLoaded    ? "loaded"
              : loaded == LoadResult::kMissing ? "not created yet"
                                               : "was corrupt");
  g_services.store(services, std::memory_order_release);
}

}  // namespace tmnt::live

using namespace tmnt;
using namespace tmnt::live;

// XamUserCreateStatsEnumerator wrappers (title, pivot, rows, spec count, specs, size*, handle*): rank with a 32-bit
// pivot (mode 1), and two with a 64-bit pivot (modes 3 and 0), both answered around the player's XUID.
REX_EXTERN(__imp__sub_826EDF58);
REX_HOOK_RAW(sub_826EDF58) {
  if (!CreateStatsEnumerator(ctx, base, StatsQuery::Kind::kRanked)) __imp__sub_826EDF58(ctx, base);
}

REX_EXTERN(__imp__sub_826EDFB8);
REX_HOOK_RAW(sub_826EDFB8) {
  if (!CreateStatsEnumerator(ctx, base, StatsQuery::Kind::kByXuid)) __imp__sub_826EDFB8(ctx, base);
}

REX_EXTERN(__imp__sub_826EE018);
REX_HOOK_RAW(sub_826EE018) {
  if (!CreateStatsEnumerator(ctx, base, StatsQuery::Kind::kByXuid)) __imp__sub_826EE018(ctx, base);
}

// XFriendsCreateEnumerator(user, start, count, size*, handle*): ReXGlue fails it, and the friends page then reports
// that the leaderboards cannot be reached.
REX_EXTERN(__imp__sub_823C7548);
REX_HOOK_RAW(sub_823C7548) {
  Services* services = Active();
  if (!services) {
    __imp__sub_823C7548(ctx, base);
    return;
  }
  ctx.r3.u64 = CreateFriendsEnumerator(base, services->enumerators, ctx.r6.u32, ctx.r7.u32);
}

// XamEnumerate wrapper (handle, buffer, size, items, overlapped): only project-owned handles are served.
REX_EXTERN(__imp__sub_826EE470);
REX_HOOK_RAW(sub_826EE470) {
  if (Services* services = Active()) {
    if (const auto query = services->enumerators.Take(ctx.r3.u32)) {
      const uint32_t overlapped = ctx.r7.u32;
      ctx.r3.u64 = ServeEnumeration(base, services->board, *query, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, overlapped);
      LogQuery(KindName(query->kind), AnswerQuery(services->board, *query), query->pivot,
               CompletionStatus(base, ctx.r3.u32, overlapped));
      return;
    }
  }
  __imp__sub_826EE470(ctx, base);
}

// XUserReadStats(title, xuid count, xuids, spec count, specs, size*, results, overlapped): the friends page reads
// its players with it. ReXGlue reports success without writing the results.
REX_EXTERN(__imp__sub_826EDE80);
REX_HOOK_RAW(sub_826EDE80) {
  const uint32_t xuid_count = ctx.r4.u32;
  const uint32_t xuids = ctx.r5.u32;
  const uint32_t spec_count = ctx.r6.u32;
  const uint32_t specs = ctx.r7.u32;
  const uint32_t size_ptr = ctx.r8.u32;
  const uint32_t results = ctx.r9.u32;
  const uint32_t overlapped = ctx.r10.u32;
  __imp__sub_826EDE80(ctx, base);
  Services* services = Active();
  if (!services || !results || !size_ptr || CompletionStatus(base, ctx.r3.u32, overlapped) != kErrorSuccess) return;

  const bool served = ServeReadStats(base, services->board, xuid_count, xuids, spec_count, specs,
                                     LoadBE32(base, size_ptr), results);
  // ReXGlue's XMsgStartIORequest stub already completed the overlapped before this point, with its own generic
  // convention (length 0 on success). Recomplete it explicitly so this result matches ServeEnumeration's.
  if (overlapped) {
    CompleteOverlapped(base, overlapped, served ? kErrorSuccess : kErrorInsufficientBuffer, served ? 1 : 0);
  } else if (!served) {
    ctx.r3.u64 = kErrorInsufficientBuffer;
  }
  LogQuery("read", AnswerReadStats(base, services->board, xuid_count, xuids, spec_count, specs), xuid_count,
           CompletionStatus(base, ctx.r3.u32, overlapped));
}

// PLT_WritePlayerSessionStats(xuid, count, leaderboard handles): the script has already stored the score in the
// registered views, and no session is needed to read it. This is the primary recorder; XSessionWriteStats below
// is a fallback.
REX_EXTERN(__imp__sub_822FDF78);
REX_HOOK_RAW(sub_822FDF78) {
  const uint64_t xuid = ctx.r3.u64;
  if (Services* services = Active()) {
    const auto writes = ParseRegisteredViews(base, LoadBE32(base, kLeaderboardRegistryGlobal), ctx.r5.u32, ctx.r4.u32);
    RecordLocalWrites(*services, xuid, writes);
  }
  __imp__sub_822FDF78(ctx, base);
}

// XSessionWriteStats(session, xuid, view count, views, overlapped): the game's own run submission.
REX_EXTERN(__imp__sub_8243CCB8);
REX_HOOK_RAW(sub_8243CCB8) {
  const uint64_t xuid = ctx.r4.u64;
  const uint32_t view_count = ctx.r5.u32;
  const uint32_t views = ctx.r6.u32;
  const auto writes = ParseViewWrites(base, views, view_count);
  __imp__sub_8243CCB8(ctx, base);

  if (Services* services = Active()) RecordLocalWrites(*services, xuid, writes);
}
