#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
  const size_t discard = hook.find("if (!g_search_tickets.Complete(ticket)) return 0;");
  CHECK(discard != std::string::npos && discard < hook.find("tmnt::WriteSearchResults(GuestHost(base, header), header"));
}

void TestNoBackgroundOnlinePump() {
  const std::string runtime = ReadSource("online_runtime.cpp");
  CHECK(!Has(runtime, "RunStalledPump") && !Has(runtime, "std::thread(") && !Has(runtime, "g_tick_mutex"));
  CHECK(!Has(ReadSource("online_bridge.cpp"), "std::thread("));
}

void TestWaitForEventsHookUsesRealWaitSemantics() {
  const std::string net = ReadSource("player_match_net_hook.cpp");
  const size_t start = net.find("void WsaWaitForEvents(");
  const std::string body = net.substr(start, net.find("\n}\n", start) - start);
  CHECK(Has(body, "wait_all ? 0 : 1") && Has(body, "kWaitTimeout") && Has(body, "kWaitIoCompletion"));
  CHECK(Has(body, "tmnt::WaitInSlices(") && Has(body, "tmnt::CompleteVdpReceives(base);"));
  CHECK(Has(net, "TMNT_NET_HOOK(8243D2C0, WsaWaitForEvents)"));
  const std::string bridge = ReadSource("online_bridge.cpp");
  CHECK(Has(bridge, "REX_HOOK_RAW(sub_82279648)") && Has(bridge, "tmnt::t_frame_thread = true;"));
  CHECK(Has(ReadSource("achievement_menu.cpp"), "REX_HOOK_RAW(sub_826EF030)"));
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
  TestEthernetLinkIsReportedActiveWhileOnline();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
