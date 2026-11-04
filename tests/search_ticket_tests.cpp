#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "lobby_trace.h"
#include "qos_placeholder.h"
#include "search_ticket.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(expr)) {                                                  \
      ++g_failures;                                                 \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    }                                                               \
  } while (0)

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SOURCE_DIR) + "/src/" + name, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestOneRequestCompletesOnce() {
  tmnt::SearchTickets tickets;
  const uint32_t first = tickets.Begin();
  CHECK(tickets.Owns(first));
  CHECK(tickets.Complete(first));
  CHECK(!tickets.Complete(first));
  CHECK(!tickets.Owns(first));
}

void TestLateAnswerOfAReleasedSearchIsDiscarded() {
  tmnt::SearchTickets tickets;
  const uint32_t first = tickets.Begin();
  const uint32_t second = tickets.Begin();
  CHECK(first != second);
  CHECK(!tickets.Owns(first));
  CHECK(!tickets.Complete(first));
  CHECK(tickets.Owns(second));
  CHECK(tickets.Complete(second));
}

void TestZeroResultSearchFollowedByASecondSearch() {
  tmnt::SearchTickets tickets;
  const uint32_t first = tickets.Begin();
  CHECK(tickets.Complete(first));
  const uint32_t second = tickets.Begin();
  CHECK(second != first && tickets.Owns(second));
  CHECK(!tickets.Complete(first));
  CHECK(tickets.Complete(second));
}

void TestQosPlaceholderIsTheOnlyPointerSkipped() {
  constexpr uint32_t kObject = 0x444F3440;
  CHECK(tmnt::IsInlineQosPlaceholder(kObject + 16, kObject));
  CHECK(!tmnt::IsInlineQosPlaceholder(kObject, kObject));
  CHECK(!tmnt::IsInlineQosPlaceholder(0x44500000, kObject));
  CHECK(!tmnt::IsInlineQosPlaceholder(0, kObject));
  CHECK(!tmnt::IsInlineQosPlaceholder(16, 0));
}

void TestHooksUseTheGuards() {
  const std::string hook = ReadSource("player_match_hook.cpp");
  const size_t qos = hook.find("void QosRelease(");
  const size_t qos_end = hook.find("\n}\n", qos);
  const std::string body = hook.substr(qos, qos_end - qos);
  CHECK(Has(body, "IsInlineQosPlaceholder(ctx.r3.u32, object)") && Has(body, "ctx.r3.u64 = 0;") &&
        Has(body, "original(ctx, base);"));
  CHECK(body.find("ctx.r3.u64 = 0;") < body.find("original(ctx, base);"));
  CHECK(Has(hook, "TMNT_LIVE_HOOK(823CB4D0, QosRelease)"));
  CHECK(Has(hook, "if (live && header && size_ptr && overlapped && max_results &&") && Has(hook, "kSearchLatencyFloor"));
  CHECK(Has(hook, "g_search_tickets.Begin()") && Has(hook, "g_search_tickets.Complete(ticket)"));
  const size_t discard = hook.find("result discarded");
  CHECK(discard != std::string::npos && discard < hook.find("tmnt::WriteSearchResults(GuestHost(base, header), header"));
}

void TestNoBackgroundOnlinePump() {
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(!Has(runtime, "RunStalledPump") && !Has(runtime, "std::thread(") && !Has(runtime, "g_tick_mutex"));
  const std::string bridge = ReadSource("online_bridge.cpp");
  CHECK(Has(bridge, "lobby heartbeat") && Has(bridge, "path={}"));
}

void TestWaitForEventsHookUsesRealWaitSemantics() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  const size_t start = net.find("void WsaWaitForEvents(");
  const std::string body = net.substr(start, net.find("\n}\n", start) - start);
  CHECK(Has(body, "wait_all ? 0 : 1") && Has(body, "kWaitTimeout") && Has(body, "kWaitIoCompletion"));
  CHECK(Has(body, "tmnt::WaitInSlices(") && Has(body, "tmnt::CompleteVdpReceives(base);"));
  CHECK(Has(net, "TMNT_NET_HOOK(8243D2C0, WsaWaitForEvents)"));
  const std::string bridge = ReadSource("online_bridge.cpp");
  CHECK(Has(ReadSource("frame_trace.cpp"), "REX_HOOK_RAW(sub_82279648)") && Has(bridge, "REX_HOOK_RAW(sub_824BE610)"));
  CHECK(Has(ReadSource("achievement_menu.cpp"), "NotePadRead"));
}

void TestFireWaitStepDiagnostics() {
  const std::string hooks = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(Has(hooks, "kFireGoWaitOffset = 0x30000 + 11828") && Has(hooks, "kFireGoStopOffset = 0x30000 + 11832"));
  CHECK(Has(hooks, "LogStep(\"DoWaitStop\", \"ENTER\"") && Has(hooks, "AFTER_ORIGINAL/EXIT"));
  CHECK(hooks.find("LogStep(\"DoWaitStop\", \"ENTER\"") < hooks.find("AFTER_ORIGINAL/EXIT"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_875C1DC8)") && Has(hooks, "REX_HOOK_RAW(sub_875D9290)"));
  CHECK(Has(hooks, "TMNT_FIRE_STEP_HOOK(875B5AD0, \"UpdateWaitStop\")"));
  CHECK(Has(hooks, "REX_HOOK_RAW(sub_875ADBC0)") && Has(hooks, "FirePkg id={}") && Has(hooks, "__imp__sub_875ADBC0(ctx, base);"));
}

void TestFrameTraceIsBoundedAndPassive() {
  const std::string trace = ReadSource("frame_trace.cpp");
  CHECK(Has(trace, "REX_HOOK_RAW(sub_82279648)") && Has(trace, "g_frames_left.load() <= 0"));
  CHECK(Has(trace, "TMNT_FRAME_STAGE(82364BC0)") && Has(trace, "not_returned"));
  CHECK(Has(trace, "DumpGuestStack();") && Has(trace, "guest return chain"));
  CHECK(Has(ReadSource("player_match_hook.cpp"), "tmnt::ArmFrameTrace(400)"));
  CHECK(!Has(ReadSource("online_bridge.cpp"), "REX_HOOK_RAW(sub_82279648)"));
}

void TestLobbyTraceFilters() {
  tmnt::RepeatFilter repeats;
  CHECK(repeats.Next("a") == 0);
  CHECK(repeats.Next("a") == -1);
  CHECK(repeats.Next("a") == -1);
  CHECK(repeats.Next("b") == 2);
  CHECK(repeats.Next("a") == 0);
  tmnt::LineBudget budget(3);
  CHECK(budget.Allow(10.0) && budget.Allow(10.1) && budget.Allow(10.2));
  CHECK(!budget.Allow(10.3) && !budget.Allow(10.9));
  CHECK(budget.TakeDropped() == 2 && budget.TakeDropped() == 0);
  CHECK(budget.Allow(11.0));
  CHECK(tmnt::IsSessionTrack("00_SessionManager_Main") && tmnt::IsSessionTrack("FireCommunicator_DferredSendCheck"));
  CHECK(!tmnt::IsSessionTrack("Leaderboard_Main") && !tmnt::IsSessionTrack(""));
  const std::string hook = ReadSource("lobby_trace_hook.cpp");
  CHECK(Has(hook, "REX_HOOK_RAW(sub_822E8308)") && Has(hook, "__imp__sub_822E8308(ctx, base);"));
}

void TestEthernetLinkIsReportedActiveWhileOnline() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  const size_t start = net.find("void EthernetLinkStatus(");
  const std::string body = net.substr(start, net.find("\n}\n", start) - start);
  CHECK(Has(body, "0x1 | 0x2 | 0x8") && Has(body, "if (!LiveNow())") && Has(body, "original(ctx, base);"));
  CHECK(Has(net, "TMNT_NET_HOOK(823CB4F0, EthernetLinkStatus)"));
}

}  // namespace

int main() {
  TestOneRequestCompletesOnce();
  TestLateAnswerOfAReleasedSearchIsDiscarded();
  TestZeroResultSearchFollowedByASecondSearch();
  TestQosPlaceholderIsTheOnlyPointerSkipped();
  TestHooksUseTheGuards();
  TestNoBackgroundOnlinePump();
  TestWaitForEventsHookUsesRealWaitSemantics();
  TestFireWaitStepDiagnostics();
  TestFrameTraceIsBoundedAndPassive();
  TestLobbyTraceFilters();
  TestEthernetLinkIsReportedActiveWhileOnline();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
