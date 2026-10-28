#include "guest_functions.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include <cstdarg>
#include <atomic>

#include "crash_report.h"
#include "guest_memory.h"

namespace tmnt {
namespace {

const PPCFuncMapping* g_mappings = nullptr;

void CapturePpcState(CrashPpcState* out) {
  if (out == nullptr) {
    return;
  }
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (thread_state == nullptr) {
    return;
  }
  PPCContext* ctx = thread_state->context();
  if (ctx == nullptr) {
    return;
  }
  const PPCRegister* const regs[kCrashReportMaxGprs] = {
      &ctx->r0,  &ctx->r1,  &ctx->r2,  &ctx->r3,  &ctx->r4,  &ctx->r5,  &ctx->r6,  &ctx->r7,
      &ctx->r8,  &ctx->r9,  &ctx->r10, &ctx->r11, &ctx->r12, &ctx->r13, &ctx->r14, &ctx->r15,
      &ctx->r16, &ctx->r17, &ctx->r18, &ctx->r19, &ctx->r20, &ctx->r21, &ctx->r22, &ctx->r23,
      &ctx->r24, &ctx->r25, &ctx->r26, &ctx->r27, &ctx->r28, &ctx->r29, &ctx->r30, &ctx->r31};
  out->valid = true;
  for (size_t i = 0; i < kCrashReportMaxGprs; ++i) {
    out->gpr[i] = regs[i]->u32;
  }
  out->lr = static_cast<uint32_t>(ctx->lr);
  out->ctr = ctx->ctr.u32;
  out->cr = (ctx->cr0.raw() << 28) | (ctx->cr1.raw() << 24) | (ctx->cr2.raw() << 20) |
            (ctx->cr3.raw() << 16) | (ctx->cr4.raw() << 12) | (ctx->cr5.raw() << 8) |
            (ctx->cr6.raw() << 4) | ctx->cr7.raw();
  out->xer = (uint32_t(ctx->xer.so) << 31) | (uint32_t(ctx->xer.ov) << 30) |
             (uint32_t(ctx->xer.ca) << 29);
  out->fpscr = ctx->fpscr.csr;
  out->f31 = ctx->f31.u64;
  out->last_indirect_target = ctx->last_indirect_target;
}

constexpr uint32_t kSingletonAddress = 0x82B59E00;
constexpr uint32_t kDescriptorC = kSingletonAddress + 0x28D18;
constexpr uint32_t kDefaultCodeStart = 0x82000000;
constexpr uint32_t kDefaultImageEnd = 0x82C40000;
constexpr uint32_t kAuxCodeStart = 0x87500000;
constexpr uint32_t kAuxImageEnd = 0x876F0000;
constexpr uint32_t kMaxDescriptorEntries = 256;
static_assert(kDescriptorC == 0x82B82B18, "C descriptor address drift");

#if defined(_WIN32)

void Append(char* buffer, size_t size, size_t& used, const char* format, ...) {
  if (buffer == nullptr || size == 0 || used + 1 >= size) {
    return;
  }
  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer + used, size - used, format, args);
  va_end(args);
  if (written <= 0) {
    return;
  }
  const size_t count = static_cast<size_t>(written);
  used += count < size - used - 1 ? count : size - used - 1;
  buffer[used] = '\0';
}

bool SafeRead32(const uint8_t* membase, uint32_t address, uint32_t* out) {
  if (membase == nullptr || out == nullptr) {
    return false;
  }
  const uint8_t* host = membase + address;
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(host, &info, sizeof(info)) != sizeof(info)) {
    return false;
  }
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return false;
  }
  const uint8_t* region_end =
      static_cast<const uint8_t*>(info.BaseAddress) + static_cast<size_t>(info.RegionSize);
  if (host + sizeof(uint32_t) > region_end) {
    return false;
  }
  *out = LoadBE32(host, 0);
  return true;
}

constexpr size_t kCSampleCount = 32;

struct CSample {
  uint32_t base = 0;
  uint32_t stride = 0;
  uint32_t growth = 0;
  uint32_t count = 0;
  uint32_t capacity = 0;
  uint32_t word0 = 0;
  uint32_t word1 = 0;
  uint64_t tick = 0;
};

CSample g_c_samples[kCSampleCount];
std::atomic<uint32_t> g_c_sample_total{0};

rex::memory::Memory* GuestMemory() {
  if (auto* kernel = rex::system::kernel_state(); kernel != nullptr) {
    if (auto* memory = kernel->memory(); memory != nullptr) {
      return memory;
    }
  }
  if (auto* thread_state = rex::runtime::ThreadState::Get(); thread_state != nullptr) {
    return thread_state->memory();
  }
  return nullptr;
}

void RecordCSample() {
  auto* memory = GuestMemory();
  if (memory == nullptr) {
    return;
  }
  const uint8_t* membase = memory->virtual_membase();
  CSample sample;
  if (!SafeRead32(membase, kDescriptorC + 0x00, &sample.base) ||
      !SafeRead32(membase, kDescriptorC + 0x04, &sample.stride) ||
      !SafeRead32(membase, kDescriptorC + 0x08, &sample.growth) ||
      !SafeRead32(membase, kDescriptorC + 0x0C, &sample.count) ||
      !SafeRead32(membase, kDescriptorC + 0x10, &sample.capacity)) {
    return;
  }
  if (sample.base != 0) {
    SafeRead32(membase, sample.base + 0, &sample.word0);
    SafeRead32(membase, sample.base + 4, &sample.word1);
  }
  sample.tick = GetTickCount64();
  const uint32_t total = g_c_sample_total.load(std::memory_order_relaxed);
  g_c_samples[total % kCSampleCount] = sample;
  g_c_sample_total.store(total + 1, std::memory_order_release);
}

const char* ClassifyPointer(uint32_t value) {
  if (value == 0) {
    return "null";
  }
  if (value < 0x10000) {
    return "small";
  }
  if (value >= kDefaultCodeStart && value < kDefaultImageEnd) {
    return "default-image";
  }
  if (value >= kAuxCodeStart && value < kAuxImageEnd) {
    return "tmnt_r-image";
  }
  return "other";
}

void CaptureGuestExtra(char* buffer, size_t size) {
  if (buffer == nullptr || size == 0) {
    return;
  }
  buffer[0] = '\0';
  auto* memory = GuestMemory();
  if (memory == nullptr) {
    return;
  }
  const uint8_t* membase = memory->virtual_membase();
  size_t used = 0;

  uint32_t base = 0;
  uint32_t stride = 0;
  uint32_t growth = 0;
  uint32_t count = 0;
  uint32_t capacity = 0;
  Append(buffer, size, used, "TMNT C: S=0x%08X descriptor=0x%08X membase=%p\n", kSingletonAddress,
         kDescriptorC, static_cast<const void*>(membase));
  if (!SafeRead32(membase, kDescriptorC + 0x00, &base) ||
      !SafeRead32(membase, kDescriptorC + 0x04, &stride) ||
      !SafeRead32(membase, kDescriptorC + 0x08, &growth) ||
      !SafeRead32(membase, kDescriptorC + 0x0C, &count) ||
      !SafeRead32(membase, kDescriptorC + 0x10, &capacity)) {
    Append(buffer, size, used, "TMNT C: descriptor unreadable\n");
    return;
  }
  Append(buffer, size, used, "TMNT C: base=0x%08X stride=%u growth=%u count=%u capacity=%u\n", base,
         stride, growth, count, capacity);
  if (base == 0 && count == 0) {
    Append(buffer, size, used, "TMNT C: inactive\n");
    return;
  }
  if (auto* thread_state = rex::runtime::ThreadState::Get();
      thread_state != nullptr && thread_state->context() != nullptr) {
    PPCContext* ctx = thread_state->context();
    Append(buffer, size, used, "TMNT C: hint r3=0x%08X lr=0x%08X ctr=0x%08X last_indirect=0x%08X\n",
           ctx->r3.u32, static_cast<uint32_t>(ctx->lr), ctx->ctr.u32, ctx->last_indirect_target);
  }

  MEMORY_BASIC_INFORMATION base_info{};
  if (base == 0) {
    Append(buffer, size, used, "TMNT C: base region=none\n");
  } else if (VirtualQuery(membase + base, &base_info, sizeof(base_info)) == sizeof(base_info)) {
    Append(buffer, size, used,
           "TMNT C: base region state=0x%X protect=0x%X size=0x%llX allocation=0x%llX\n",
           static_cast<unsigned>(base_info.State), static_cast<unsigned>(base_info.Protect),
           static_cast<unsigned long long>(base_info.RegionSize),
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base_info.AllocationBase)));
  } else {
    Append(buffer, size, used, "TMNT C: base region unqueryable\n");
  }

  uint32_t words[4] = {};
  if (base != 0) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!SafeRead32(membase, base + i * 4, &words[i])) {
        words[i] = 0;
      }
    }
    Append(buffer, size, used, "TMNT C: base words=%08X %08X %08X %08X\n", words[0], words[1],
           words[2], words[3]);
  }

  const uint32_t total_samples = g_c_sample_total.load(std::memory_order_acquire);
  if (total_samples != 0) {
    const uint32_t samples = total_samples < kCSampleCount ? total_samples : kCSampleCount;
    const uint32_t first = total_samples - samples;
    Append(buffer, size, used, "TMNT C: history=%u samples\n", samples);
    for (uint32_t i = 0; i < samples; ++i) {
      const CSample& sample = g_c_samples[(first + i) % kCSampleCount];
      Append(buffer, size, used,
             "TMNT C: t=%llu base=%08X count=%u capacity=%u first=%08X %08X\n",
             static_cast<unsigned long long>(sample.tick), sample.base, sample.count,
             sample.capacity, sample.word0, sample.word1);
    }
  } else {
    Append(buffer, size, used, "TMNT C: history=none\n");
  }

  const uint32_t entries = count < kMaxDescriptorEntries ? count : kMaxDescriptorEntries;
  Append(buffer, size, used, "TMNT C: entries=%u%s\n", entries,
         count > kMaxDescriptorEntries ? " (truncated at 256)" : "");
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t slot = base + i * stride;
    uint32_t entry = 0;
    if (!SafeRead32(membase, slot, &entry)) {
      Append(buffer, size, used, "TMNT C[%u]=unreadable\n", i);
      continue;
    }
    uint32_t vptr = 0;
    if (entry != 0 && SafeRead32(membase, entry, &vptr)) {
      Append(buffer, size, used, "TMNT C[%u]=%08X vptr=%08X [%s]\n", i, entry, vptr,
             ClassifyPointer(vptr));
    } else {
      Append(buffer, size, used, "TMNT C[%u]=%08X vptr=unreadable\n", i, entry);
    }
  }
}

#else

void CaptureGuestExtra(char* buffer, size_t size) {
  (void)buffer;
  (void)size;
}

#endif

void ResolveGuestFunction(uint64_t host_address, CrashGuestRef* out) {
  if (out == nullptr || g_mappings == nullptr || host_address == 0) {
    return;
  }
  uint64_t function_start = host_address;
#if defined(_WIN32) && defined(_M_X64)
  DWORD64 image_base = 0;
  if (const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(host_address, &image_base, nullptr); entry != nullptr) {
    function_start = image_base + entry->BeginAddress;
  }
#endif
  const PPCFuncMapping* match = nullptr;
  bool ambiguous = false;
  for (const PPCFuncMapping* mapping = g_mappings; mapping->guest != 0 || mapping->host != nullptr; ++mapping) {
    if (mapping->host == nullptr || reinterpret_cast<uint64_t>(mapping->host) != function_start) {
      continue;
    }
    if (match != nullptr) {
      ambiguous = true;
      break;
    }
    match = mapping;
  }
  if (match == nullptr || ambiguous) {
    return;
  }
  out->address = static_cast<uint32_t>(match->guest);
  std::snprintf(out->name, sizeof(out->name), "sub_%08X", out->address);
  out->known = true;
}

}  // namespace

void SampleCrashGuestState() {
#if defined(_WIN32)
  RecordCSample();
#endif
}

void InstallGuestFunctionTable(const PPCFuncMapping* mappings) {
  g_mappings = mappings;
  SetCrashGuestFunctionResolver(&ResolveGuestFunction);
  SetCrashPpcStateProvider(&CapturePpcState);
  SetCrashGuestExtraProvider(&CaptureGuestExtra);
}

}  // namespace tmnt
