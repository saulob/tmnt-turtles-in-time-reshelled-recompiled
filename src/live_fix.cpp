#include "live_fix.h"

#include <atomic>
#include <cstring>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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
constexpr int kMaxLoggedOverrides = 8;

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

void RecordWrite(Services& services, uint64_t xuid, const ViewWrite& write) {
  const RecordResult result = services.stats.Record(xuid, write.view, write.stats);
  REXLOG_INFO("[Stats] local score view={} rating={} {}", write.view, write.stats.rating,
              result == RecordResult::kSaved       ? "saved"
              : result == RecordResult::kUnchanged ? "unchanged"
                                                   : "not saved");
}

void RecordLocalWrites(Services& services, uint64_t xuid, const std::vector<ViewWrite>& writes) {
  for (const ViewWrite& write : writes) {
    if (xuid != services.board.user().xuid) {
      REXLOG_WARN("[Stats] write for xuid {:016X}, local is {:016X}", xuid, services.board.user().xuid);
      continue;
    }
    if (write.stats.rating > 0) RecordWrite(services, xuid, write);
  }
}

// A zeroed, permanently-allocated guest buffer substituted for sub_823C9B50's missing per-row
// value-table pointer (see the hook below). Lazily allocated once; never freed.
uint32_t EmptyLeaderboardValueTable(uint8_t* base) {
  static std::atomic<uint32_t> table{0};
  if (uint32_t address = table.load(std::memory_order_acquire)) return address;
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return 0;
  constexpr uint32_t kSize = 4096;
  uint32_t address = memory->SystemHeapAlloc(kSize);
  if (!address) return 0;
  std::memset(base + address, 0, kSize);
  uint32_t expected = 0;
  if (!table.compare_exchange_strong(expected, address, std::memory_order_acq_rel)) {
    memory->SystemHeapFree(address);
    return expected;
  }
  return address;
}

}  // namespace

void Install(InstallParams params) {
  auto* services = new Services(std::move(params.stats_file), std::move(params.user));
  const LoadResult loaded = services->stats.Load();
  REXLOG_INFO("[Stats] Local stats file {}",
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

// sub_823C9B50(spec, row, stat, dest, bound): formats one leaderboard column value for a row,
// resolved through spec's registered-view descriptor chain (spec+24 -> link, link+4 -> meta,
// meta+12 -> table_base; the same registered-view table as kLeaderboardRegistryGlobal). Its
// per-row record (table_base + row*48) carries a secondary per-stat value-table pointer at byte
// offset 44, which the original reloads from its own stack spill and dereferences unconditionally
// (confirmed byte-exact against the recompiled PPC at guest 0x823C9BD8/0x823C9BF4). Every other
// field it validates first (id, row count, stat count) is already correct; only this one pointer
// is ever left null, when the local/offline profile has not written a score for this view this
// session. Replicate exactly the same guard chain the original uses before it reaches that read,
// and only when every guard already passed AND the pointer is still 0, substitute a permanent
// zeroed guest buffer so the read lands on valid memory instead of guest address 0. The game's own
// downstream type-tag/threshold checks then either reject the column cleanly or format it as a
// zero value - both are the game's own existing "no data" representations, not ones we invent.
REX_EXTERN(__imp__sub_823C9B50);
REX_HOOK_RAW(sub_823C9B50) {
  const uint32_t spec = ctx.r3.u32;
  const uint32_t row = ctx.r4.u32;
  const uint32_t stat = ctx.r5.u32;
  if (spec) {
    const uint32_t id0 = LoadBE32(base, spec + 0);
    const uint32_t id1 = LoadBE32(base, spec + 4);
    const uint32_t link = LoadBE32(base, spec + 24);
    const uint32_t inner = link ? LoadBE32(base, link + 0) : 0;
    const uint32_t meta = (link && inner) ? LoadBE32(base, link + 4) : 0;
    const uint32_t meta_id = meta ? LoadBE32(base, meta + 0) : 0;
    const uint32_t meta_count = meta ? LoadBE32(base, meta + 8) : 0;
    if (meta && meta_id == id0 && meta_count > row && stat < id1) {
      const uint32_t table_base = LoadBE32(base, meta + 12);
      if (table_base) {
        const uint32_t value_ptr_slot = table_base + row * 48 + 44;
        if (LoadBE32(base, value_ptr_slot) == 0) {
          if (const uint32_t fallback = EmptyLeaderboardValueTable(base)) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < kMaxLoggedOverrides) {
              REXLOG_INFO(
                  "[Leaderboard] empty row substituted: sub_823C9B50 row={} stat={} "
                  "value_ptr_slot={:#x} payload_ptr={:#x} status=empty_row",
                  row, stat, value_ptr_slot, fallback);
            }
            StoreBE32(base, value_ptr_slot, fallback);
          }
        }
      }
    }
  }
  __imp__sub_823C9B50(ctx, base);
}
