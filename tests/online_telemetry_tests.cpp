// Client telemetry collector: allow-listed events, the value sanitizer, bounded queue and batch, rate limit, retry,
// peer reports and the privacy guarantee over every produced batch. In memory: no network.
// Run: tmnt_online_telemetry_tests

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "online/telemetry.h"
#include "updater/json.h"

using namespace tmnt::online;
namespace json = trg::update::json;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

const char* const kAllEvents[] = {
    "invite_received",   "invite_failed",      "join_started",       "join_completed",
    "join_failed",       "game_start_attempt", "game_start_success", "game_start_failure",
    "worker_reconnect",  "direct_attempt",     "direct_result",      "relay_fallback",
    "relay_result",      "transport_selected", "peer_disconnect",    "session_end"};

TelemetryContext Context() {
  TelemetryContext c;
  c.game_id = "tmnt-reshelled";
  c.diag = "0123456789ab";
  c.port_version = "0.4.2";
  c.platform = "windows";
  c.os = "10.0.26200";
  c.arch = "x64";
  c.fingerprint = "9f2c41aa7be0d311";
  c.protocol = 3;
  c.slot = 2;
  c.host = false;
  c.party_size = 3;
  return c;
}

json::Value Parse(const std::string& text) {
  std::string error;
  auto value = json::Parse(text, &error);
  CHECK(value.has_value());
  return value ? *value : json::Value{};
}

const json::Value* Events(const json::Value& batch) {
  const json::Value* e = batch.Find("events");
  return e && e->IsArray() ? e : nullptr;
}

// Every privacy pattern, searched over a whole batch (not only over the values).
void CheckPrivate(const std::string& text) {
  static const std::regex friend_code("[A-Z0-9]{4}-[A-Z0-9]{4}");
  static const std::regex hex_token("[0-9a-fA-F]{32,}");
  static const std::regex ipv4("[0-9]{1,3}(\\.[0-9]{1,3}){3}");
  static const std::regex ipv6("([0-9a-fA-F]{0,4}:){2,}");
  static const std::regex url("://|www\\.|mailto:");
  CHECK(!std::regex_search(text, friend_code));
  CHECK(!std::regex_search(text, hex_token));
  CHECK(!std::regex_search(text, ipv4));
  CHECK(!std::regex_search(text, ipv6));
  CHECK(!std::regex_search(text, url));
  CHECK(text.find('/') == std::string::npos);
  CHECK(text.find('\\') == std::string::npos);
  CHECK(text.find('@') == std::string::npos);
  CHECK(text.find(' ') == std::string::npos);
  CHECK(text.size() <= Telemetry::kMaxBatchBytes);
  CHECK(json::Parse(text).has_value());
}

void TestAllowList() {
  Telemetry t;
  for (const char* name : kAllEvents) t.Event(name);
  CHECK(t.pending_events() == 16);
  CHECK(t.rejected_names() == 0);

  const size_t before = t.pending_events();
  t.Event("not_an_event");
  t.Event("Invite_Received");
  t.Event("");
  t.Event("invite_received ");
  t.Event("ABCD-EFGH");
  t.Event("http://evil.example/");
  CHECK(t.pending_events() == before);
  CHECK(t.rejected_names() == 6);

  auto text = t.TakeBatch(0);
  CHECK(text.has_value());
  const json::Value batch = Parse(*text);
  const json::Value* events = Events(batch);
  CHECK(events && events->AsArray().size() == 16);
  CHECK(text->find("not_an_event") == std::string::npos);
  CheckPrivate(*text);
}

void TestSanitizer() {
  CHECK(SanitizeTelemetryValue("direct_timeout") == "direct_timeout");
  CHECK(SanitizeTelemetryValue("failed:hole_punch_failed") == "failed:hole_punch_failed");
  CHECK(SanitizeTelemetryValue("v1.2.3-rc1") == "v1.2.3-rc1");
  CHECK(SanitizeTelemetryValue("") == "");
  CHECK(SanitizeTelemetryValue("12") == "12");
  CHECK(SanitizeTelemetryValue("1.2.3") == "1.2.3");  // three groups are not an address

  CHECK(SanitizeTelemetryValue("Q7K2-9ZXA") == "redacted");
  CHECK(SanitizeTelemetryValue("code=Q7K2-9ZXA!") == "redacted");
  CHECK(SanitizeTelemetryValue("Q7K2-9ZXAB") == "redacted");  // embedded in something longer
  CHECK(SanitizeTelemetryValue("q7k2-9zxa") == "q7k2-9zxa");  // codes are uppercase
  CHECK(SanitizeTelemetryValue("AB!CD-EFGH") == "redacted");  // joined by dropping the "!"

  const std::string token64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  CHECK(SanitizeTelemetryValue(token64) == "redacted");
  CHECK(SanitizeTelemetryValue("tok_" + token64.substr(0, 32)) == "redacted");
  CHECK(SanitizeTelemetryValue(token64.substr(0, 31)) == token64.substr(0, 31));
  CHECK(SanitizeTelemetryValue("0123456789ABCDEF0123456789ABCDEF") == "redacted");
  CHECK(SanitizeTelemetryValue("01234567-89ab-cdef-0123-456789abcdef") == "redacted");  // dashed uuid

  CHECK(SanitizeTelemetryValue("192.168.1.20") == "redacted");
  CHECK(SanitizeTelemetryValue("ip192.168.1.20x") == "redacted");
  CHECK(SanitizeTelemetryValue("8.8.8.8:3478") == "redacted");
  CHECK(SanitizeTelemetryValue("1.2.3.4.5") == "redacted");
  CHECK(SanitizeTelemetryValue("2001:db8::1") == "redacted");
  CHECK(SanitizeTelemetryValue("::1") == "redacted");
  CHECK(SanitizeTelemetryValue("fe80::1%eth0") == "redacted");
  CHECK(SanitizeTelemetryValue("[2001:db8::1]:7001") == "redacted");
  CHECK(SanitizeTelemetryValue("::ffff:1.2.3.4") == "redacted");

  CHECK(SanitizeTelemetryValue("http://evil.example/x") == "redacted");
  CHECK(SanitizeTelemetryValue("https://worker.example.com") == "redacted");
  CHECK(SanitizeTelemetryValue("wss://host") == "redacted");
  CHECK(SanitizeTelemetryValue("www.example.com") == "redacted");
  CHECK(SanitizeTelemetryValue("WWW.EXAMPLE.COM") == "redacted");
  CHECK(SanitizeTelemetryValue("user@example.com") == "redacted");

  CHECK(SanitizeTelemetryValue("C:\\Users\\Saulo\\game.exe") == "redacted");
  CHECK(SanitizeTelemetryValue("/home/saulo/.config") == "redacted");
  CHECK(SanitizeTelemetryValue("a/b") == "redacted");

  CHECK(SanitizeTelemetryValue("John Smith") == "redacted");  // free text, whitespace
  CHECK(SanitizeTelemetryValue("two\tparts") == "redacted");
  CHECK(SanitizeTelemetryValue("line\nbreak") == "redacted");
  CHECK(SanitizeTelemetryValue(std::string_view("nul\0byte", 8)) == "redacted");
  CHECK(SanitizeTelemetryValue("Jo\xC3\xA3o") == "Joo");  // non-ASCII is dropped
  CHECK(SanitizeTelemetryValue("a\"b<c>{d}") == "abcd");

  const std::string long_value(500, 'z');
  CHECK(SanitizeTelemetryValue(long_value) == std::string(48, 'z'));
  CHECK(SanitizeTelemetryValue(std::string(100000, 'q')).size() == 48);
  CHECK(SanitizeTelemetryValue(std::string(100000, '/')) == "redacted");

  for (const std::string& reason : {std::string(reason::direct_timeout), std::string(reason::service_public_endpoints_unsupported),
                                    std::string(reason::establishment_timeout)}) {
    CHECK(SanitizeTelemetryValue(reason) == reason.substr(0, 48));
  }
  CHECK(reason::stun_failed == "stun_failed" && reason::offline == "offline" && reason::busy == "busy");
  CHECK(reason::no_public_endpoint == "no_public_endpoint" && reason::host_left == "host_left");
}

void TestFieldsAndTime() {
  Telemetry t;
  t.SetContext(Context(), 100.0);
  t.Event("invite_received", {{"to_slot", "2"}, {"why", "friend:Q7K2-9ZXA"}, {"from", "10.0.0.1"}}, 101.25);
  t.Event("session_end", {}, 99.0);  // before the epoch: clamped
  auto text = t.TakeBatch(110);
  CHECK(text.has_value());
  const json::Value batch = Parse(*text);
  const json::Value* events = Events(batch);
  CHECK(events && events->AsArray().size() == 2);
  const json::Value* first = events->At(0);
  CHECK(first && first->Find("e")->AsString() == "invite_received");
  CHECK(first && Near(first->Find("t")->AsNumber(), 1.25));
  const json::Value* f = first ? first->Find("f") : nullptr;
  CHECK(f && f->Find("to_slot") && f->Find("to_slot")->AsString() == "2");
  CHECK(f && f->Find("why")->AsString() == "redacted");
  CHECK(f && f->Find("from")->AsString() == "redacted");
  const json::Value* second = events->At(1);
  CHECK(second && Near(second->Find("t")->AsNumber(), 0.0) && second->Find("f") == nullptr);

  // Without a time on SetContext the first Event/TakeBatch time is the zero.
  Telemetry lazy;
  lazy.SetContext(Context());
  lazy.Event("join_started", {}, 5000.5);
  lazy.Event("join_completed", {}, 5002.0);
  const json::Value lb = Parse(*lazy.TakeBatch(5003));
  CHECK(Near(Events(lb)->At(0)->Find("t")->AsNumber(), 0.0));
  CHECK(Near(Events(lb)->At(1)->Find("t")->AsNumber(), 1.5));

  // At most 8 fields, empty keys skipped.
  Telemetry many;
  many.Event("direct_result", {{"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "1"}, {"e", "1"}, {"f", "1"}, {"g", "1"},
                               {"h", "1"}, {"i", "1"}, {"j", "1"}});
  many.Event("direct_result", {{"", "x"}, {" ", "y"}, {"k", "v"}});
  const json::Value mb = Parse(*many.TakeBatch(0));
  CHECK(Events(mb)->At(0)->Find("f")->AsObject().size() == 8);
  CHECK(Events(mb)->At(1)->Find("f")->AsObject().size() == 2);  // "k" and the redacted " "
}

void TestQueueBound() {
  Telemetry t;
  for (int i = 0; i < 250; ++i) t.Event("join_completed", {{"n", std::to_string(i)}});
  CHECK(t.pending_events() == Telemetry::kMaxQueued);
  CHECK(t.dropped_events() == 50);
  auto text = t.TakeBatch(0);
  CHECK(text.has_value());
  const json::Value batch = Parse(*text);
  const json::Value* events = Events(batch);
  // 200 events do not fit 8 KB: the queue bound dropped 50, the size bound drops more, always the oldest.
  CHECK(events && !events->AsArray().empty() && events->AsArray().size() < 200);
  const size_t n = events->AsArray().size();
  CHECK(events->At(n - 1)->Find("f")->Find("n")->AsString() == "249");
  CHECK(events->At(0)->Find("f")->Find("n")->AsString() == std::to_string(250 - n));
  CHECK(batch.Find("dropped") && batch.Find("dropped")->AsInt64() == static_cast<int64_t>(250 - n));
  CHECK(t.pending_events() == 0);

  // The loss is reported once.
  t.Event("join_completed");
  const json::Value next = Parse(*t.TakeBatch(100));
  CHECK(next.Find("dropped") == nullptr);
  CHECK(t.dropped_events() == 250 - n);
  CheckPrivate(*text);
}

void TestBatchSizeBound() {
  Telemetry t;
  t.SetContext(Context(), 0);
  const std::string wide(48, 'z');
  for (int i = 0; i < 40; ++i) {
    t.Event("direct_result",
            {{"k0", wide}, {"k1", wide}, {"k2", wide}, {"k3", wide}, {"k4", wide}, {"k5", wide}, {"k6", wide},
             {"k7", std::to_string(i)}},
            i);
  }
  CHECK(t.pending_events() == 40);
  auto text = t.TakeBatch(100);
  CHECK(text.has_value());
  CHECK(text->size() <= Telemetry::kMaxBatchBytes);
  CHECK(text->size() > Telemetry::kMaxBatchBytes - 1500);  // it filled up, it did not just drop everything
  const json::Value batch = Parse(*text);
  const json::Value* events = Events(batch);
  CHECK(events && !events->AsArray().empty() && events->AsArray().size() < 40);
  const size_t n = events->AsArray().size();
  CHECK(events->At(n - 1)->Find("f")->Find("k7")->AsString() == "39");  // newest survives
  const int64_t dropped = batch.Find("dropped") ? *batch.Find("dropped")->AsInt64() : 0;
  CHECK(dropped == static_cast<int64_t>(40 - n));
  CHECK(t.dropped_events() == static_cast<uint32_t>(dropped));
  CHECK(t.pending_events() == 0);

  // Reports crowding the batch never push it over the bound either.
  Telemetry r;
  r.SetContext(Context(), 0);
  for (int local = 1; local <= 4; ++local) {
    for (int remote = 1; remote <= 4; ++remote) {
      PeerReport p;
      p.local_slot = local;
      p.remote_slot = remote;
      p.state = wide;
      p.transport = wide;
      p.direct_result = wide;
      p.relay_result = wide;
      p.fallback_reason = wide;
      p.disconnect_reason = wide;
      r.SetPeer(p);
    }
  }
  for (int i = 0; i < 100; ++i) r.Event("direct_result", {{"k0", wide}, {"k1", wide}});
  auto big = r.TakeBatch(0);
  CHECK(big.has_value() && big->size() <= Telemetry::kMaxBatchBytes);
  CHECK(json::Parse(*big).has_value());
}

void TestRateLimitAndForce() {
  Telemetry t;
  CHECK(!t.TakeBatch(0).has_value());  // nothing yet
  CHECK(!t.TakeBatch(0, true).has_value());
  t.Event("worker_reconnect");
  CHECK(t.TakeBatch(1.0).has_value());  // the first batch is not delayed
  CHECK(!t.TakeBatch(1.0).has_value());  // nothing new

  t.Event("invite_received");
  CHECK(!t.TakeBatch(5.0).has_value());   // too soon
  CHECK(t.pending_events() == 1);         // and nothing lost
  CHECK(!t.TakeBatch(10.99).has_value());
  CHECK(t.TakeBatch(11.0).has_value());   // exactly the interval later

  t.Event("join_failed");
  CHECK(t.TakeBatch(11.5, true).has_value());  // force ignores the limit
  t.Event("invite_failed");
  CHECK(!t.TakeBatch(12.0).has_value());       // and restarts it
  CHECK(t.TakeBatch(21.5).has_value());

  // A report alone is news.
  SessionReport s;
  s.duration_ms = 1234;
  t.SetSession(s);
  CHECK(!t.TakeBatch(22.0).has_value());
  CHECK(t.TakeBatch(40.0).has_value());
  CHECK(!t.TakeBatch(80.0).has_value());  // unchanged since

  // Hostile clocks do not throw or wedge it.
  t.Event("session_end");
  CHECK(t.TakeBatch(std::nan("")).has_value() || true);
  t.Event("session_end");
  CHECK(t.TakeBatch(-5.0, true).has_value());
}

void TestRestoreBatch() {
  Telemetry t;
  t.Event("invite_received", {{"n", "1"}});
  t.Event("invite_received", {{"n", "2"}});
  auto first = t.TakeBatch(0);
  CHECK(first.has_value() && t.pending_events() == 0);

  t.RestoreBatch("some other text");  // not the batch it handed out
  CHECK(t.pending_events() == 0);

  t.RestoreBatch(*first);
  CHECK(t.pending_events() == 2);
  t.RestoreBatch(*first);  // already restored
  CHECK(t.pending_events() == 2);

  t.Event("invite_failed", {{"n", "3"}});
  auto second = t.TakeBatch(1.0);  // too soon but the retry is only rate limited like any batch
  CHECK(!second.has_value());
  second = t.TakeBatch(10.0);
  CHECK(second.has_value());
  const json::Value sb = Parse(*second);
  CHECK(Events(sb)->AsArray().size() == 3);
  CHECK(Events(sb)->At(0)->Find("f")->Find("n")->AsString() == "1");  // restored ones keep their place
  CHECK(Events(sb)->At(2)->Find("f")->Find("n")->AsString() == "3");

  t.RestoreBatch(*second);  // fails a second time: the twice-failed events are dropped, the fresh one stays
  CHECK(t.pending_events() == 1);
  CHECK(t.dropped_events() == 2);
  auto third = t.TakeBatch(30.0);
  CHECK(third.has_value());
  const json::Value tb = Parse(*third);
  CHECK(Events(tb)->AsArray().size() == 1);
  CHECK(Events(tb)->At(0)->Find("f")->Find("n")->AsString() == "3");
  CHECK(tb.Find("dropped") && tb.Find("dropped")->AsInt64() == 2);
  t.RestoreBatch(*third);  // it was restored once already, so a third delivery failure ends it
  t.RestoreBatch(*third);
  CHECK(t.pending_events() == 0);
  CHECK(t.dropped_events() == 3);

  // Losses that were in a failed batch are reported by the next one, not lost.
  Telemetry u;
  for (int i = 0; i < 205; ++i) u.Event("join_completed");
  auto overflowed = u.TakeBatch(0);
  CHECK(Parse(*overflowed).Find("dropped")->AsInt64() == 5);
  u.RestoreBatch(*overflowed);
  CHECK(u.dropped_events() == 5);
  auto again = u.TakeBatch(10);
  CHECK(again.has_value() && Parse(*again).Find("dropped")->AsInt64() == 5);

  // Restore is bounded by the queue size.
  Telemetry v;
  for (int i = 0; i < 200; ++i) v.Event("join_completed", {{"n", std::to_string(i)}});
  auto full = v.TakeBatch(0);
  for (int i = 0; i < 200; ++i) v.Event("join_completed");
  v.RestoreBatch(*full);
  CHECK(v.pending_events() == Telemetry::kMaxQueued);
  CHECK(v.dropped_events() == 200);

  // A restored batch still brings the reports back into the next one.
  Telemetry w;
  PeerReport p;
  p.local_slot = 1;
  p.remote_slot = 2;
  p.state = "connected";
  w.SetPeer(p);
  auto with_peer = w.TakeBatch(0);
  CHECK(!w.TakeBatch(100).has_value());
  w.RestoreBatch(*with_peer);
  auto retry = w.TakeBatch(100);
  CHECK(retry.has_value() && Parse(*retry).Find("peers") != nullptr);
}

void TestJsonShape() {
  Telemetry t;
  TelemetryContext c = Context();
  c.host = true;
  t.SetContext(c, 0);

  SessionReport s;
  s.duration_ms = 61000;
  s.connection_test_usable = true;
  s.worker_reachable = true;
  s.stun_ok = true;
  s.direct_available = false;
  s.relay_available = true;
  s.compatibility = "ok";
  s.disconnect_reason = std::string(reason::peer_disconnected);
  t.SetSession(s);

  PeerReport p;
  p.local_slot = 1;
  p.remote_slot = 2;
  p.state = "connected";
  p.transport = "relay";
  p.direct_result = "failed:" + std::string(reason::hole_punch_failed);
  p.relay_result = "ok";
  p.fallback_reason = std::string(reason::direct_timeout);
  p.connected_ms = 45000;
  p.ping.sent = 40;
  p.ping.received = 38;
  p.ping.last_ms = 52;
  p.ping.min_ms = 40;
  p.ping.max_ms = 90;
  p.ping.avg_ms = 55.26;
  p.ping.jitter_ms = 3.04;
  p.ping.timeouts = 2;
  p.ping.out_of_order = 1;
  p.ping.duplicates = 3;
  p.sent_packets = 1000;
  p.received_packets = 990;
  p.reconnects = 1;
  p.disconnect_reason = "";
  p.establishment_ms = 4200;
  t.SetPeer(p);
  t.Event("transport_selected", {{"transport", "relay"}}, 4.2);

  const std::string diag = Telemetry::NewDiagId();
  auto text = t.TakeBatch(5.0);
  CHECK(text.has_value());
  CheckPrivate(*text);
  const json::Value b = Parse(*text);
  CHECK(b.IsObject());
  CHECK(b.Find("op") && b.Find("op")->AsString() == "telemetry.batch");
  CHECK(b.Find("diag") && b.Find("diag")->AsString() == "0123456789ab");
  const json::Value* ctx = b.Find("ctx");
  CHECK(ctx && ctx->Find("game")->AsString() == "tmnt-reshelled");
  CHECK(ctx && ctx->Find("proto")->AsInt64() == 3);
  CHECK(ctx && ctx->Find("version")->AsString() == "0.4.2");
  CHECK(ctx && ctx->Find("platform")->AsString() == "windows" && ctx->Find("os")->AsString() == "10.0.26200");
  CHECK(ctx && ctx->Find("arch")->AsString() == "x64" && ctx->Find("fp")->AsString() == "9f2c41aa7be0d311");
  CHECK(ctx && ctx->Find("slot")->AsInt64() == 2 && ctx->Find("host")->AsBool() && ctx->Find("party")->AsInt64() == 3);

  const json::Value* session = b.Find("session");
  CHECK(session && session->Find("dur_ms")->AsInt64() == 61000);
  CHECK(session && session->Find("ct_usable")->AsBool() && session->Find("relay")->AsBool());
  CHECK(session && !session->Find("direct")->AsBool());
  CHECK(session && session->Find("compat")->AsString() == "ok");
  CHECK(session && session->Find("reason")->AsString() == "peer_disconnected");

  const json::Value* peers = b.Find("peers");
  CHECK(peers && peers->IsArray() && peers->AsArray().size() == 1);
  const json::Value* peer = peers ? peers->At(0) : nullptr;
  CHECK(peer && peer->Find("ls")->AsInt64() == 1 && peer->Find("rs")->AsInt64() == 2);
  CHECK(peer && peer->Find("transport")->AsString() == "relay");
  CHECK(peer && peer->Find("direct")->AsString() == "failed:hole_punch_failed");
  CHECK(peer && peer->Find("fallback")->AsString() == "direct_timeout");
  CHECK(peer && peer->Find("conn_ms")->AsInt64() == 45000 && peer->Find("est_ms")->AsInt64() == 4200);
  const json::Value* ping = peer ? peer->Find("ping") : nullptr;
  CHECK(ping && ping->Find("sent")->AsInt64() == 40 && ping->Find("recv")->AsInt64() == 38);
  CHECK(ping && ping->Find("min")->AsInt64() == 40 && ping->Find("max")->AsInt64() == 90);
  CHECK(ping && Near(ping->Find("avg")->AsNumber(), 55.3) && Near(ping->Find("jit")->AsNumber(), 3.0));
  CHECK(ping && ping->Find("to")->AsInt64() == 2 && ping->Find("ooo")->AsInt64() == 1);
  CHECK(ping && ping->Find("dup")->AsInt64() == 3);
  CHECK(ping && Near(ping->Find("loss")->AsNumber(), 2.0 / 40.0, 0.001));
  CHECK(peer && peer->Find("tx")->AsInt64() == 1000 && peer->Find("rx")->AsInt64() == 990);
  CHECK(peer && peer->Find("reconn")->AsInt64() == 1);

  const json::Value* events = Events(b);
  CHECK(events && events->AsArray().size() == 1);
  CHECK(events && Near(events->At(0)->Find("t")->AsNumber(), 4.2));
  CHECK(b.Find("dropped") == nullptr);

  // Reports stay in later batches until changed; events do not.
  t.Event("session_end");
  const json::Value later = Parse(*t.TakeBatch(20.0));
  CHECK(later.Find("session") && later.Find("peers"));
  CHECK(Events(later)->AsArray().size() == 1);

  // Not-a-number statistics never produce invalid JSON.
  PeerReport bad = p;
  bad.ping.avg_ms = std::nan("");
  bad.ping.jitter_ms = INFINITY;
  t.SetPeer(bad);
  auto badtext = t.TakeBatch(40.0);
  CHECK(badtext.has_value() && json::Parse(*badtext).has_value());

  // Never-set context still yields a valid batch.
  Telemetry bare;
  bare.Event("session_end");
  auto bt = bare.TakeBatch(0);
  CHECK(bt.has_value() && json::Parse(*bt).has_value());
}

void TestPeerReports() {
  Telemetry t;
  PeerReport p;
  p.local_slot = 1;
  p.remote_slot = 2;
  p.state = "connecting";
  t.SetPeer(p);
  p.state = "connected";
  p.transport = "direct";
  t.SetPeer(p);  // replaces
  PeerReport q = p;
  q.remote_slot = 3;
  q.state = "lost";
  t.SetPeer(q);

  const json::Value b = Parse(*t.TakeBatch(0));
  const json::Value* peers = b.Find("peers");
  CHECK(peers && peers->AsArray().size() == 2);
  CHECK(peers->At(0)->Find("rs")->AsInt64() == 2 && peers->At(0)->Find("state")->AsString() == "connected");
  CHECK(peers->At(1)->Find("rs")->AsInt64() == 3 && peers->At(1)->Find("state")->AsString() == "lost");

  // Slots outside 1-4 are ignored.
  Telemetry u;
  PeerReport bad;
  for (int slot : {0, 5, -1, 99}) {
    bad.local_slot = slot;
    bad.remote_slot = 1;
    u.SetPeer(bad);
    bad.local_slot = 1;
    bad.remote_slot = slot;
    u.SetPeer(bad);
  }
  CHECK(!u.TakeBatch(0).has_value());

  // Bounded number of distinct reports.
  Telemetry v;
  for (int l = 1; l <= 4; ++l) {
    for (int r = 1; r <= 4; ++r) {
      PeerReport x;
      x.local_slot = l;
      x.remote_slot = r;
      v.SetPeer(x);
    }
  }
  const json::Value vb = Parse(*v.TakeBatch(0));
  CHECK(vb.Find("peers")->AsArray().size() == Telemetry::kMaxPeers);
  PeerReport existing;
  existing.local_slot = 1;
  existing.remote_slot = 1;
  existing.state = "left";
  v.SetPeer(existing);  // known keys can still be updated
  const json::Value vb2 = Parse(*v.TakeBatch(100));
  CHECK(vb2.Find("peers")->At(0)->Find("state")->AsString() == "left");
}

void TestHostileEverywhere() {
  const std::string token64 = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
  const std::vector<std::string> hostile = {
      "ABCD-EFGH",
      "Q7K2-9ZXA",
      "friend Q7K2-9ZXA here",
      token64,
      "Bearer " + token64,
      "01234567-89ab-cdef-0123-456789abcdef",
      "192.168.0.1",
      "10.0.0.1:7001",
      "177.18.65.229",
      "2001:db8::8a2e:370:7334",
      "::1",
      "fe80::1%eth0",
      "[::ffff:192.0.2.1]:80",
      "http://example.com/path?token=abc",
      "https://worker.example.workers.dev/ws",
      "wss://x.y",
      "www.example.com",
      "C:\\Users\\Saulo\\AppData\\x.log",
      "/home/saulo/.local/x",
      "\\\\server\\share",
      "John Smith",
      "  leading and trailing  ",
      "saulo@example.com",
      "\"quote\" and \\ backslash",
      "{\"a\":1}",
      "],\"dropped\":0,\"evil\":[",
      "\n\r\t\x01\x7F",
      std::string("nul\0inside", 10),
      "Jo\xC3\xA3o \xF0\x9F\x90\xA2",
      "AB!CD-EFGH",
      "AB CD-EFGH",
      "dead:beef:cafe",
      std::string(20000, 'A'),
      std::string(20000, '/'),
      std::string(60, '1') + ".2.3.4",
      "",
  };

  Telemetry t;
  for (const std::string& h : hostile) {
    TelemetryContext c;
    c.game_id = c.diag = c.port_version = c.platform = c.os = c.arch = c.fingerprint = h;
    t.SetContext(c, 0);

    SessionReport s;
    s.compatibility = h;
    s.disconnect_reason = h;
    t.SetSession(s);

    PeerReport p;
    p.local_slot = 1;
    p.remote_slot = 2;
    p.state = p.transport = p.direct_result = p.relay_result = p.fallback_reason = p.disconnect_reason = h;
    t.SetPeer(p);

    t.Event(h);  // as a name: rejected
    t.Event("direct_result", {{h, "ok"}, {"reason", h}, {"other", h}}, 1.0);

    for (bool force : {true}) {
      auto text = t.TakeBatch(1.0, force);
      CHECK(text.has_value());
      if (text) CheckPrivate(*text);
    }
  }
  CHECK(t.rejected_names() == hostile.size() - 0);  // none of them is an allowed name

  // And a batch carrying all of them at once, restored and taken again.
  Telemetry u;
  for (const std::string& h : hostile) u.Event("peer_disconnect", {{"a", h}, {"b", h}});
  auto text = u.TakeBatch(0);
  CHECK(text.has_value());
  if (text) {
    CheckPrivate(*text);
    u.RestoreBatch(*text);
    auto again = u.TakeBatch(100);
    CHECK(again.has_value());
    if (again) CheckPrivate(*again);
  }

  // Events with absurd times.
  Telemetry w;
  w.Event("session_end", {}, 1e300);
  w.Event("session_end", {}, -1e300);
  w.Event("session_end", {}, INFINITY);
  w.Event("session_end", {}, std::nan(""));
  auto wt = w.TakeBatch(0);
  CHECK(wt.has_value());
  if (wt) CheckPrivate(*wt);
}

void TestDiagId() {
  const std::regex hex12("^[0-9a-f]{12}$");
  std::vector<std::string> seen;
  for (int i = 0; i < 50; ++i) {
    const std::string id = Telemetry::NewDiagId();
    CHECK(id.size() == 12);
    CHECK(std::regex_match(id, hex12));
    CHECK(SanitizeTelemetryValue(id) == id);
    seen.push_back(id);
  }
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

void TestPingStatsIntegration() {
  // The collector carries whatever the pinger measured.
  PeerPing ping;
  std::vector<uint8_t> reply;
  auto wire = ping.Due(2, 0);
  PeerPing remote;
  remote.Take(1, wire->data(), wire->size(), 0, reply);
  ping.Take(2, reply.data(), reply.size(), 37, reply);
  ping.Due(2, 1000);
  ping.Due(2, 4500);

  Telemetry t;
  PeerReport p;
  p.local_slot = 1;
  p.remote_slot = 2;
  p.ping = ping.stats(2);
  t.SetPeer(p);
  const json::Value b = Parse(*t.TakeBatch(0));
  const json::Value* pg = b.Find("peers")->At(0)->Find("ping");
  CHECK(pg->Find("sent")->AsInt64() == 3 && pg->Find("recv")->AsInt64() == 1);
  CHECK(pg->Find("last")->AsInt64() == 37 && pg->Find("to")->AsInt64() == 1);
  CHECK(Near(pg->Find("loss")->AsNumber(), 0.5, 0.001));
}

}  // namespace

std::map<std::string, std::string> SharedReasons() {
  std::map<std::string, std::string> out;
  std::ifstream in(std::string(TMNT_SOURCE_DIR) + "/tests/reason-codes.txt");
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    out[line.substr(0, space)] = line.substr(space + 1);
  }
  return out;
}

void TestReasonTableMatchesTheSharedList() {
  const std::map<std::string, std::string> shared = SharedReasons();
  CHECK(!shared.empty());
  std::map<std::string, std::string> own;
  for (const ReasonEntry& entry : kReasons) {
    CHECK(own.emplace(std::string(entry.code), std::string(entry.category)).second);
    CHECK(SanitizeTelemetryValue(entry.code) == entry.code);
    CHECK(!std::regex_search(std::string(entry.code), std::regex("[^a-z_]")));
  }
  CHECK(own == shared);
  CHECK(NormalizeReason("direct_timeout") == "direct_timeout" && ReasonCategory("direct_timeout") == "path");
  CHECK(ReasonCategory("host_left") == "disconnect" && ReasonCategory("version_mismatch") == "service");
  CHECK(ReasonCategory("declined") == "invite" && ReasonCategory("worker_unreachable") == "connection_test");
  CHECK(NormalizeReason("something_new") == "other" && ReasonCategory("something_new").empty());
  CHECK(reason::relay_receive_overflow == "relay_receive_overflow" && reason::chooser_chose_relay == "chooser_chose_relay");
}

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SOURCE_DIR) + "/src/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void TestEveryReasonLiteralInTheSourcesIsKnown() {
  struct Scan {
    const char* file;
    const char* statement;
    const char* prefix;
  };
  const Scan scans[] = {
      {"online/match_session.cpp", R"X(FailLink\([^,]+,\s*("[a-z_]+"))X", ""},
      {"online/match_session.cpp", R"X(RequestRelay\([^,]+,\s*("[a-z_]+"))X", ""},
      {"online/match_session.cpp", R"X(why = ([^;]*);)X", ""},
      {"online/match_session.cpp", R"X(\{"reason",\s*("[a-z_]+")\})X", ""},
      {"netplay/online_game_socket.cpp", R"X(failure = ("[a-z_]+"))X", "relay_"},
      {"online/online_service.cpp", R"X(EndParty\(("[a-z_]+")\))X", ""},
      {"online/online_service.cpp", R"X(\{"reason",\s*("[a-z_]+")\})X", ""},
  };
  const std::regex literal(R"X("([a-z_]+)")X");
  size_t checked = 0;
  for (const Scan& scan : scans) {
    const std::string text = ReadSource(scan.file);
    CHECK(!text.empty());
    const std::regex pattern(scan.statement);
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
      const std::string statement = (*it)[1].str();
      for (std::sregex_iterator lit(statement.begin(), statement.end(), literal), lend; lit != lend; ++lit) {
        const std::string code = std::string(scan.prefix) + (*lit)[1].str();
        if (code.back() == '_') continue;
        ++checked;
        CHECK(!ReasonCategory(code).empty());
        if (ReasonCategory(code).empty()) std::printf("  unknown reason %s in %s\n", code.c_str(), scan.file);
      }
    }
  }
  CHECK(checked >= 20);
}

void TestUnknownReasonsAreBoundedAndNeverLost() {
  Telemetry t;
  t.SetContext(Context(), 0);
  t.Event("direct_result", {{"result", "failed"}, {"reason", "direct_timeout"}});
  t.Event("peer_disconnect", {{"reason", "brand_new_reason"}});
  t.Event("peer_disconnect", {{"reason", "friend:Q7K2-9ZXA"}});
  CHECK(t.unknown_reasons() == 2);

  SessionReport session;
  session.disconnect_reason = "also_unknown";
  t.SetSession(session);
  PeerReport peer;
  peer.local_slot = 1;
  peer.remote_slot = 2;
  peer.fallback_reason = "chooser_chose_relay";
  peer.disconnect_reason = "mystery";
  peer.direct_result = "failed:no_such_code";
  peer.relay_result = "ok";
  t.SetPeer(peer);
  CHECK(t.unknown_reasons() == 5);

  const json::Value batch = Parse(*t.TakeBatch(1));
  const json::Value* events = Events(batch);
  CHECK(events && events->AsArray().size() == 3);
  CHECK(events->At(0)->Find("f")->Find("reason")->AsString() == "direct_timeout");
  CHECK(events->At(1)->Find("f")->Find("reason")->AsString() == "other");
  CHECK(events->At(2)->Find("f")->Find("reason")->AsString() == "other");
  CHECK(batch.Find("session")->Find("reason")->AsString() == "other");
  const json::Value* p = batch.Find("peers")->At(0);
  CHECK(p->Find("fallback")->AsString() == "chooser_chose_relay" && p->Find("reason")->AsString() == "other");
  CHECK(p->Find("direct")->AsString() == "failed:other" && p->Find("relay")->AsString() == "ok");
}

void TestEventNumbersAreMonotonicAndSurviveARetry() {
  Telemetry t;
  t.SetContext(Context(), 0);
  t.Event("join_started");
  t.Event("join_completed");
  t.Event("invite_received");
  const std::string first = *t.TakeBatch(0);
  const json::Value a = Parse(first);
  CHECK(Events(a)->AsArray().size() == 3);
  for (size_t i = 0; i < 3; ++i) CHECK(Events(a)->At(i)->Find("n")->AsInt64() == static_cast<int64_t>(i + 1));

  t.Event("session_end");
  t.RestoreBatch(first);
  const json::Value b = Parse(*t.TakeBatch(20));
  const json::Value* events = Events(b);
  CHECK(events->AsArray().size() == 4);
  const int64_t expected[] = {1, 2, 3, 4};
  for (size_t i = 0; i < 4; ++i) CHECK(events->At(i)->Find("n")->AsInt64() == expected[i]);

  t.Event("join_failed");
  const json::Value c = Parse(*t.TakeBatch(40));
  CHECK(Events(c)->AsArray().size() == 1 && Events(c)->At(0)->Find("n")->AsInt64() == 5);
}

void TestContextBeforeAnyPartyHasSlotZero() {
  Telemetry t;
  TelemetryContext c = Context();
  c.slot = 0;
  c.host = false;
  c.party_size = 0;
  t.SetContext(c, 0);
  t.Event("invite_received");
  const json::Value batch = Parse(*t.TakeBatch(1));
  CHECK(batch.Find("ctx")->Find("slot")->AsInt64() == 0 && batch.Find("ctx")->Find("party")->AsInt64() == 0);
  CHECK(batch.Find("diag")->AsString() == c.diag);

  SessionReport session;
  t.SetSession(session);
  t.ClearReports();
  t.Event("join_started");
  const json::Value next = Parse(*t.TakeBatch(30));
  CHECK(next.Find("session") == nullptr && next.Find("peers") == nullptr);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  TestReasonTableMatchesTheSharedList();
  TestEveryReasonLiteralInTheSourcesIsKnown();
  TestUnknownReasonsAreBoundedAndNeverLost();
  TestEventNumbersAreMonotonicAndSurviveARetry();
  TestContextBeforeAnyPartyHasSlotZero();
  TestAllowList();
  TestSanitizer();
  TestFieldsAndTime();
  TestQueueBound();
  TestBatchSizeBound();
  TestRateLimitAndForce();
  TestRestoreBatch();
  TestJsonShape();
  TestPeerReports();
  TestHostileEverywhere();
  TestDiagId();
  TestPingStatsIntegration();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
