// DEBUG_SAULO: while ticks are slow, samples what the frame thread is parked on and which threads burn the CPU.
#include "frame_block_sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/ppc.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace tmnt {

#if defined(_WIN32)

namespace {

constexpr int kSampleMs = 2;
constexpr int kThreadReportSamples = 400;
constexpr size_t kSites = 128;
constexpr size_t kReportedSites = 6;
constexpr size_t kReportedThreads = 5;
constexpr size_t kChainDepth = 2;
constexpr uintptr_t kSyscallStubBytes = 32;

enum Parked : uint32_t { kLock, kEvent, kMulti, kSleep, kYield, kOnCpu, kParkedCount };
constexpr std::array<const char*, kParkedCount> kParkedNames{"lock", "event", "multi", "sleep", "yield", "on_cpu"};
constexpr std::array<const char*, kParkedCount> kStubs{"NtWaitForAlertByThreadId", "NtWaitForSingleObject",
                                                       "NtWaitForMultipleObjects", "NtDelayExecution",
                                                       "NtYieldExecution", nullptr};

struct Site {
  uint32_t parked = 0;
  uint32_t guest_lr = 0;
  std::array<uint32_t, kChainDepth> callers{};
  uint32_t samples = 0;
};

struct ThreadCpu {
  DWORD id = 0;
  uint64_t cpu_100ns = 0;
};

std::atomic<PPCContext*> g_ctx{nullptr};
std::atomic<uint8_t*> g_base{nullptr};
std::atomic<DWORD> g_thread_id{0};
std::atomic<bool> g_active{false};
std::atomic<bool> g_started{false};

std::mutex g_mutex;
std::array<Site, kSites> g_sites;
std::array<uint64_t, kParkedCount> g_parked{};
std::string g_threads;

bool ReadGuest32(const uint8_t* base, uint32_t address, uint32_t& value) {
  uint8_t bytes[4];
  SIZE_T read = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), base + address, bytes, sizeof(bytes), &read) || read != sizeof(bytes)) {
    return false;
  }
  value = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
  return true;
}

// The frame thread is suspended here, possibly inside the heap: nothing below may allocate.
Site ReadGuestSite(const PPCContext& ctx, const uint8_t* base) {
  Site site;
  site.guest_lr = uint32_t(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  for (size_t depth = 0; depth < kChainDepth; ++depth) {
    uint32_t back = 0;
    if (!ReadGuest32(base, sp, back) || back <= sp) break;
    if (!ReadGuest32(base, back - 8, site.callers[depth])) break;
    sp = back;
  }
  return site;
}

uint32_t Classify(uintptr_t rip, const std::array<uintptr_t, kParkedCount>& stubs) {
  for (uint32_t i = 0; i < kParkedCount; ++i) {
    if (stubs[i] && rip >= stubs[i] && rip < stubs[i] + kSyscallStubBytes) return i;
  }
  return kOnCpu;
}

void Count(const Site& sample) {
  const auto same = [&](const Site& site) {
    return site.samples && site.parked == sample.parked && site.guest_lr == sample.guest_lr &&
           site.callers == sample.callers;
  };
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_parked[sample.parked];
  const size_t start = (sample.guest_lr ^ sample.callers[0] ^ sample.parked) % kSites;
  for (size_t i = 0; i < kSites; ++i) {
    Site& site = g_sites[(start + i) % kSites];
    if (same(site)) {
      ++site.samples;
      return;
    }
    if (!site.samples) {
      site = sample;
      site.samples = 1;
      return;
    }
  }
}

void ReportBusyThreads(std::vector<ThreadCpu>& previous) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  std::vector<ThreadCpu> current;
  std::vector<std::pair<uint64_t, std::string>> busy;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    FILETIME created, ended, kernel, user;
    if (GetThreadTimes(thread, &created, &ended, &kernel, &user)) {
      const auto ticks = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
      const uint64_t cpu = ticks(kernel) + ticks(user);
      current.push_back({entry.th32ThreadID, cpu});
      const auto before = std::find_if(previous.begin(), previous.end(),
                                       [&](const ThreadCpu& t) { return t.id == entry.th32ThreadID; });
      const uint64_t spent = cpu - (before == previous.end() ? cpu : before->cpu_100ns);
      if (spent) {
        std::string name = fmt::format("t{}", entry.th32ThreadID);
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
          for (const wchar_t* c = description; *c; ++c) {
            if (c == description) name += ':';
            name += char(*c < 128 ? *c : '?');
          }
          LocalFree(description);
        }
        busy.emplace_back(spent, std::move(name));
      }
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  previous = std::move(current);
  std::sort(busy.rbegin(), busy.rend());
  std::string text;
  for (size_t i = 0; i < std::min(busy.size(), kReportedThreads); ++i) {
    text += fmt::format("{}{}={}", i ? "," : "", busy[i].second, busy[i].first / 10000);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_threads = std::move(text);
}

void Sampler() {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  std::array<uintptr_t, kParkedCount> stubs{};
  for (size_t i = 0; i < kParkedCount; ++i) {
    if (kStubs[i]) stubs[i] = reinterpret_cast<uintptr_t>(GetProcAddress(ntdll, kStubs[i]));
  }
  const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   g_thread_id.load());
  if (!thread) return;
  std::vector<ThreadCpu> cpu;
  int until_threads = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
    if (!g_active.load()) continue;
    if (SuspendThread(thread) == DWORD(-1)) return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    const bool have_context = GetThreadContext(thread, &context) != 0;
    Site sample = ReadGuestSite(*g_ctx.load(), g_base.load());
    ResumeThread(thread);
    if (have_context) {
      sample.parked = Classify(uintptr_t(context.Rip), stubs);
      Count(sample);
    }
    if (--until_threads <= 0) {
      until_threads = kThreadReportSamples;
      ReportBusyThreads(cpu);
    }
  }
}

}  // namespace

bool FrameSamplerEnabled() {
  static const bool enabled = std::getenv("TMNT_FRAME_BLOCK_SAMPLER") != nullptr;
  return enabled;
}

void NoteFrameThreadForSampler(PPCContext* ctx, uint8_t* base) {
  if (!FrameSamplerEnabled() || g_started.load(std::memory_order_relaxed)) return;
  g_ctx = ctx;
  g_base = base;
  g_thread_id = GetCurrentThreadId();
  g_started = true;
  std::thread(Sampler).detach();
}

void SetFrameSamplerActive(bool active) { g_active = active; }

std::string FrameBlockSummary() {
  std::array<Site, kSites> sites;
  std::array<uint64_t, kParkedCount> parked;
  std::string threads;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sites = std::exchange(g_sites, {});
    parked = std::exchange(g_parked, {});
    threads = std::move(g_threads);
    g_threads.clear();
  }
  std::string text = "parked";
  for (size_t i = 0; i < kParkedCount; ++i) text += fmt::format(" {}={}", kParkedNames[i], parked[i]);
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.samples > b.samples; });
  text += " sites";
  for (size_t i = 0; i < kReportedSites && sites[i].samples; ++i) {
    text += fmt::format(" {}@{:08X}<{:08X}<{:08X}={}", kParkedNames[sites[i].parked], sites[i].guest_lr,
                        sites[i].callers[0], sites[i].callers[1], sites[i].samples);
  }
  return text + " thread_cpu_ms " + threads;
}

#else

bool FrameSamplerEnabled() { return false; }
void NoteFrameThreadForSampler(PPCContext*, uint8_t*) {}
void SetFrameSamplerActive(bool) {}
std::string FrameBlockSummary() { return {}; }

#endif

}  // namespace tmnt
