// The engine's language bank loader: under pt-BR the English bank it just parsed takes its text from the port's table.
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "localization/active_language.h"
#include "localization/game_text.h"

namespace {

// The table sub_82291888 returns.
constexpr uint32_t kBankRecords = 0;
constexpr uint32_t kBankRecordCount = 4;
constexpr uint32_t kBankPool = 8;
constexpr uint32_t kBankPoolChars = 12;
constexpr uint32_t kBankNarrowPool = 16;
constexpr uint32_t kRecordBytes = 28;
constexpr uint32_t kRecordId = 0;
constexpr uint32_t kRecordOffset = 4;
constexpr uint32_t kNoString = 0xFFFFFFFF;

// The engine's heap: the bank's pools must come from it, because its unload frees them there.
constexpr uint32_t kHeapPointer = 0x82AC33BC;

}  // namespace

REX_EXTERN(__imp__sub_824B4A00);
REX_EXTERN(__imp__sub_824B4F28);

namespace {

uint32_t Load32(uint8_t* base, uint32_t address) { return tmnt::LoadBE32(tmnt::GuestHost(base, address), 0); }

void Store32(uint8_t* base, uint32_t address, uint32_t value) { tmnt::StoreBE32(tmnt::GuestHost(base, address), 0, value); }

uint32_t HeapAlloc(PPCContext& ctx, uint8_t* base, uint32_t bytes) {
  ctx.r3.u64 = Load32(base, kHeapPointer);
  ctx.r4.u64 = bytes;
  __imp__sub_824B4A00(ctx, base);
  return ctx.r3.u32;
}

void HeapFree(PPCContext& ctx, uint8_t* base, uint32_t address) {
  ctx.r3.u64 = Load32(base, kHeapPointer);
  ctx.r4.u64 = address;
  __imp__sub_824B4F28(ctx, base);
}

std::u16string ReadString(uint8_t* base, uint32_t pool, uint32_t offset, uint32_t pool_chars) {
  std::u16string text;
  for (uint32_t i = offset; i < pool_chars; ++i) {
    const char16_t value = char16_t(tmnt::LoadBE16(tmnt::GuestHost(base, pool + i * 2), 0));
    if (value == u'\0') break;
    text.push_back(value);
  }
  return text;
}

void LocalizeBank(PPCContext& ctx, uint8_t* base, uint32_t table) {
  const uint32_t count = Load32(base, table + kBankRecordCount);
  const uint32_t records = Load32(base, table + kBankRecords);
  const uint32_t old_pool = Load32(base, table + kBankPool);
  const uint32_t old_narrow = Load32(base, table + kBankNarrowPool);
  const uint32_t pool_chars = Load32(base, table + kBankPoolChars);
  if (count == 0 || records == 0 || old_pool == 0) return;
  const auto bank = tmnt::localization::BankForFirstId(Load32(base, records + kRecordId));
  if (!bank) return;

  std::vector<std::optional<std::u16string>> english;
  english.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = Load32(base, records + i * kRecordBytes + kRecordOffset);
    if (offset == kNoString) {
      english.emplace_back();
    } else {
      english.emplace_back(ReadString(base, old_pool, offset, pool_chars));
    }
  }
  const tmnt::localization::LocalizedBank localized = tmnt::localization::BuildLocalizedBank(
      *bank, english, tmnt::localization::PortugueseBrazilGameText().rows);

  const uint32_t chars = uint32_t(localized.pool.size());
  const uint32_t pool = HeapAlloc(ctx, base, chars * 2);
  const uint32_t narrow = pool ? HeapAlloc(ctx, base, chars) : 0;
  if (!pool || !narrow) {
    REXLOG_WARN("Language: pt-BR game text not applied, out of guest memory");
    return;
  }
  for (uint32_t i = 0; i < chars; ++i) {
    const char16_t unit = localized.pool[i];
    tmnt::StoreBE16(tmnt::GuestHost(base, pool + i * 2), 0, uint16_t(unit));
    *tmnt::GuestHost(base, narrow + i) = uint8_t(unit);
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (localized.offsets[i]) Store32(base, records + i * kRecordBytes + kRecordOffset, *localized.offsets[i]);
  }
  Store32(base, table + kBankPool, pool);
  Store32(base, table + kBankPoolChars, chars);
  Store32(base, table + kBankNarrowPool, narrow);
  HeapFree(ctx, base, old_pool);
  if (old_narrow) HeapFree(ctx, base, old_narrow);
  REXLOG_INFO("Language: pt-BR game text bank={} replaced={} kept={} stale={}", int(*bank), localized.replaced,
              localized.kept, localized.stale);
}

}  // namespace

REX_EXTERN(__imp__sub_82291888);
REX_HOOK_RAW(sub_82291888) {
  __imp__sub_82291888(ctx, base);
  const uint32_t table = ctx.r3.u32;
  if (table == 0 || !tmnt::localization::PortugueseBrazilSelected()) return;
  LocalizeBank(ctx, base, table);
  ctx.r3.u64 = table;
}
