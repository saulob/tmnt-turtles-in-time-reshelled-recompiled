// Peer round-trip measurement: the 13-byte ping/pong datagram, per-slot statistics, loss, staleness. Pure logic.
// Run: tmnt_online_peer_ping_tests

#include <cmath>
#include <cstdio>
#include <vector>

#include "online/peer_ping.h"

using namespace tmnt::online;

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

// Sends a ping to `slot` at `sent`, and returns the pong the peer would answer with.
std::vector<uint8_t> PongFor(PeerPing& local, int slot, uint64_t sent) {
  auto ping = local.Due(slot, sent);
  std::vector<uint8_t> reply;
  PeerPing remote;
  if (ping) remote.Take(1, ping->data(), ping->size(), sent, reply);
  return reply;
}

bool Deliver(PeerPing& p, int slot, const std::vector<uint8_t>& wire, uint64_t now) {
  std::vector<uint8_t> reply;
  return p.Take(slot, wire.data(), wire.size(), now, reply);
}

// Round trip of exactly `rtt` ms started at `sent`.
void Sample(PeerPing& p, int slot, uint64_t sent, uint64_t rtt) {
  auto pong = PongFor(p, slot, sent);
  Deliver(p, slot, pong, sent + rtt);
}

void TestWire() {
  PeerPing p;
  auto ping = p.Due(2, 0x01020304);
  CHECK(ping.has_value());
  CHECK(ping->size() == 13);
  const std::vector<uint8_t> want = {'T', 'P', 'N', 'G', 1, 0, 0, 0, 1, 0x01, 0x02, 0x03, 0x04};
  CHECK(*ping == want);
  CHECK(PeerPing::LooksLikePing(ping->data(), ping->size()));

  PeerPing remote;
  std::vector<uint8_t> reply;
  CHECK(remote.Take(1, ping->data(), ping->size(), 5, reply));
  const std::vector<uint8_t> pong = {'T', 'P', 'N', 'G', 2, 0, 0, 0, 1, 0x01, 0x02, 0x03, 0x04};
  CHECK(reply == pong);
  CHECK(PeerPing::LooksLikePing(reply.data(), reply.size()));

  auto second = p.Due(2, 0x01020304 + 1000);
  CHECK(second.has_value());
  CHECK((*second)[8] == 2);  // seq 2, big-endian

  // The sender time is a 32-bit wrap of the 64-bit clock.
  PeerPing q;
  auto wrapped = q.Due(1, 0x1'0000'0007ull);
  CHECK(wrapped.has_value() && (*wrapped)[9] == 0 && (*wrapped)[12] == 7);
}

void TestRoundTrip() {
  PeerPing p;
  CHECK(!p.rtt_ms(1).has_value());
  auto pong = PongFor(p, 1, 1000);
  CHECK(!pong.empty());
  CHECK(Deliver(p, 1, pong, 1042));
  PingStats st = p.stats(1);
  CHECK(st.sent == 1 && st.received == 1);
  CHECK(st.last_ms == 42 && st.min_ms == 42 && st.max_ms == 42);
  CHECK(Near(st.avg_ms, 42.0));
  CHECK(st.pending == 0);
  CHECK(p.rtt_ms(1) == 42);
  CHECK(!p.rtt_ms(2).has_value());  // other slots are independent
  CHECK(p.stats(2).sent == 0);

  // Due: first immediately, then every second.
  PeerPing d;
  CHECK(d.Due(1, 5000).has_value());
  CHECK(!d.Due(1, 5000).has_value());
  CHECK(!d.Due(1, 5999).has_value());
  CHECK(d.Due(1, 6000).has_value());
  CHECK(!d.Due(1, 6500).has_value());
  CHECK(d.Due(1, 7000).has_value());
  // A peer that never answers is pinged all the same, and every slot has its own timer.
  CHECK(d.Due(3, 7000).has_value());
}

void TestLossAndTimeouts() {
  PeerPing p;
  CHECK(p.Due(1, 0).has_value());
  CHECK(p.stats(1).pending == 1);
  CHECK(Near(p.stats(1).loss(), 0.0));  // nothing settled yet
  CHECK(p.Due(1, 1000).has_value());
  CHECK(p.Due(1, 2000).has_value());
  CHECK(p.Due(1, 3000).has_value());
  CHECK(p.stats(1).timeouts == 0);  // exactly kTimeoutMs old is not expired
  CHECK(p.Due(1, 3001).has_value() == false);
  CHECK(p.stats(1).timeouts == 1);
  CHECK(p.Due(1, 4001).has_value());
  CHECK(p.stats(1).timeouts == 2);
  PingStats st = p.stats(1);
  CHECK(st.sent == 5 && st.pending == 3);
  CHECK(Near(st.loss(), 2.0 / 2.0));  // 2 timeouts of 2 settled

  // A late pong for an expired ping is ignored, not a sample and not a duplicate.
  PeerPing late;
  auto ping = late.Due(1, 0);
  late.Due(1, 3500);
  CHECK(late.stats(1).timeouts == 1);
  std::vector<uint8_t> reply;
  PeerPing remote;
  remote.Take(1, ping->data(), ping->size(), 0, reply);
  CHECK(Deliver(late, 1, reply, 3600));
  CHECK(late.stats(1).received == 0 && late.stats(1).duplicates == 0);

  // Half answered.
  PeerPing half;
  Sample(half, 1, 0, 10);
  half.Due(1, 1000);
  half.Due(1, 5000);  // the second one is lost; also sends a third
  PingStats h = half.stats(1);
  CHECK(h.sent == 3 && h.received == 1 && h.timeouts == 1 && h.pending == 1);
  CHECK(Near(h.loss(), 0.5));
  CHECK(Near(PingStats{}.loss(), 0.0));
}

void TestDuplicateAndOutOfOrder() {
  PeerPing p;
  auto pong1 = PongFor(p, 1, 0);
  auto pong2 = PongFor(p, 1, 1000);
  auto pong3 = PongFor(p, 1, 2000);

  CHECK(Deliver(p, 1, pong1, 2010));
  CHECK(Deliver(p, 1, pong3, 2020));
  CHECK(p.stats(1).out_of_order == 0);
  CHECK(Deliver(p, 1, pong2, 2030));  // seq 2 after seq 3
  CHECK(p.stats(1).out_of_order == 1);
  CHECK(p.stats(1).received == 3);

  CHECK(Deliver(p, 1, pong3, 2040));  // answered twice
  CHECK(Deliver(p, 1, pong1, 2050));
  PingStats st = p.stats(1);
  CHECK(st.duplicates == 2);
  CHECK(st.received == 3);
  CHECK(st.out_of_order == 1);

  // Unknown seq: consumed, ignored.
  const std::vector<uint8_t> stray = {'T', 'P', 'N', 'G', 2, 0, 0, 0x99, 0x99, 0, 0, 0, 1};
  CHECK(Deliver(p, 1, stray, 2060));
  CHECK(p.stats(1).received == 3 && p.stats(1).duplicates == 2);

  // Right seq but a time we never sent under it.
  PeerPing q;
  auto ping = q.Due(1, 1000);
  std::vector<uint8_t> forged = *ping;
  forged[4] = 2;
  forged[12] ^= 1;
  CHECK(Deliver(q, 1, forged, 1010));
  CHECK(q.stats(1).received == 0 && q.stats(1).pending == 1);
}

void TestAggregates() {
  PeerPing p;
  const int rtts[] = {20, 40, 20, 60};
  uint64_t t = 0;
  for (int r : rtts) {
    Sample(p, 1, t, static_cast<uint64_t>(r));
    t += 1000;
  }
  PingStats st = p.stats(1);
  CHECK(st.received == 4);
  CHECK(st.last_ms == 60 && st.min_ms == 20 && st.max_ms == 60);
  CHECK(Near(st.avg_ms, 35.0));
  // 1/16 smoothing of |20|, |20|, |40|.
  double j = 0;
  j += (20 - j) / 16;
  j += (20 - j) / 16;
  j += (40 - j) / 16;
  CHECK(Near(st.jitter_ms, j));
  CHECK(st.jitter_ms > 0);

  // Constant round trip: no jitter.
  PeerPing steady;
  for (int i = 0; i < 6; ++i) Sample(steady, 1, i * 1000, 30);
  CHECK(Near(steady.stats(1).jitter_ms, 0.0));
  CHECK(steady.stats(1).min_ms == 30 && steady.stats(1).max_ms == 30);
}

void TestMedianAndStaleness() {
  PeerPing p;
  const int rtts[] = {100, 10, 30, 20, 40, 50};
  uint64_t t = 0;
  for (int i = 0; i < 6; ++i) {
    Sample(p, 1, t, static_cast<uint64_t>(rtts[i]));
    t += 1000;
    if (i == 0) CHECK(p.rtt_ms(1) == 100);
    if (i == 2) CHECK(p.rtt_ms(1) == 30);  // 100 10 30 -> median 30
  }
  // Window is the last 5: 10 30 20 40 50 -> 30 (the 100 fell out).
  CHECK(p.rtt_ms(1) == 30);
  CHECK(p.stats(1).max_ms == 100);  // the lifetime stats still remember it

  const uint64_t last_answer = 5000 + 50;
  p.Due(1, last_answer + 5000);
  CHECK(p.rtt_ms(1).has_value());   // exactly 5 s: still shown
  p.Due(1, last_answer + 5001);
  CHECK(!p.rtt_ms(1).has_value());  // nothing answered for more than 5 s

  // An answer revives it.
  auto pong = PongFor(p, 1, last_answer + 6000);
  Deliver(p, 1, pong, last_answer + 6025);
  CHECK(p.rtt_ms(1) == 30);  // window 30 20 40 50 25 -> sorted 20 25 30 40 50
}

void TestReset() {
  PeerPing p;
  Sample(p, 1, 0, 25);
  Sample(p, 2, 0, 35);
  p.Due(1, 1000);
  p.Reset(1);
  PingStats st = p.stats(1);
  CHECK(st.sent == 0 && st.received == 0 && st.min_ms == -1 && st.pending == 0);
  CHECK(!p.rtt_ms(1).has_value());
  CHECK(p.rtt_ms(2) == 35);  // other slots untouched
  auto fresh = p.Due(1, 1001);  // new connection: pinged at once, seq restarts
  CHECK(fresh.has_value() && (*fresh)[8] == 1);
  p.Reset(0);
  p.Reset(5);
  p.Reset(-3);
}

void TestGarbage() {
  PeerPing p;
  std::vector<uint8_t> reply = {9};
  const std::vector<uint8_t> good = {'T', 'P', 'N', 'G', 1, 0, 0, 0, 1, 0, 0, 0, 0};

  auto bad_magic = good;
  bad_magic[0] = 'X';
  CHECK(!PeerPing::LooksLikePing(bad_magic.data(), bad_magic.size()));
  CHECK(!p.Take(1, bad_magic.data(), bad_magic.size(), 0, reply));

  auto kind3 = good;
  kind3[4] = 3;
  CHECK(!PeerPing::LooksLikePing(kind3.data(), kind3.size()));
  CHECK(!p.Take(1, kind3.data(), kind3.size(), 0, reply));
  auto kind0 = good;
  kind0[4] = 0;
  CHECK(!PeerPing::LooksLikePing(kind0.data(), kind0.size()));

  auto shorter = good;
  shorter.pop_back();
  CHECK(!PeerPing::LooksLikePing(shorter.data(), shorter.size()));
  CHECK(!p.Take(1, shorter.data(), shorter.size(), 0, reply));
  auto longer = good;
  longer.push_back(0);
  CHECK(!PeerPing::LooksLikePing(longer.data(), longer.size()));
  CHECK(!p.Take(1, longer.data(), longer.size(), 0, reply));
  CHECK(!PeerPing::LooksLikePing(good.data(), 0));
  CHECK(!PeerPing::LooksLikePing(nullptr, 13));
  CHECK(!p.Take(1, nullptr, 0, 0, reply));
  CHECK(reply.size() == 1 && reply[0] == 9);  // untouched on rejection
  CHECK(PeerPing::LooksLikePing(good.data(), good.size()));

  // A game packet must never look like a ping.
  const std::vector<uint8_t> game(13, 0);
  CHECK(!PeerPing::LooksLikePing(game.data(), game.size()));
  CHECK(p.stats(1).sent == 0 && p.stats(1).received == 0);
}

void TestSlotRange() {
  PeerPing p;
  std::vector<uint8_t> reply;
  const std::vector<uint8_t> ping = {'T', 'P', 'N', 'G', 1, 0, 0, 0, 1, 0, 0, 0, 0};
  for (int slot : {0, 5, -1, 100}) {
    CHECK(!p.Due(slot, 0).has_value());
    CHECK(!p.Take(slot, ping.data(), ping.size(), 0, reply));
    CHECK(!p.rtt_ms(slot).has_value());
    CHECK(p.stats(slot).sent == 0);
  }
  CHECK(p.Take(4, ping.data(), ping.size(), 0, reply));  // slot 4 is valid
  CHECK(reply.size() == 13 && reply[4] == 2);
  CHECK(p.Due(4, 0).has_value());

  // A ping from a peer is always answered, with or without our own traffic.
  std::vector<uint8_t> again;
  CHECK(p.Take(4, ping.data(), ping.size(), 10, again) && again == reply);
  CHECK(p.stats(4).received == 0);
}

}  // namespace

void TestFourPeersKeepIndependentState() {
  PeerPing p;
  std::vector<uint8_t> pongs[5];
  for (int slot = 2; slot <= 4; ++slot) pongs[slot] = PongFor(p, slot, 1000);
  CHECK(!p.rtt_ms(2) && !p.rtt_ms(3) && !p.rtt_ms(4));

  // Answers arrive out of order, one peer late, one never; a pong never satisfies another slot.
  CHECK(Deliver(p, 4, pongs[4], 1030));
  CHECK(p.rtt_ms(4) == 30 && !p.rtt_ms(2) && !p.rtt_ms(3));
  CHECK(!p.rtt_ms(2));
  CHECK(Deliver(p, 2, pongs[2], 1200));
  CHECK(p.rtt_ms(2) == 200 && p.rtt_ms(4) == 30 && !p.rtt_ms(3));
  CHECK(p.stats(3).received == 0 && p.stats(3).pending == 1 && p.stats(2).received == 1 && p.stats(4).received == 1);

  // The silent peer times out on its own and costs the others nothing.
  CHECK(p.Due(3, 1000 + PeerPing::kTimeoutMs + 1).has_value());
  CHECK(p.stats(3).timeouts == 1 && p.stats(2).timeouts == 0 && p.stats(4).timeouts == 0);
  CHECK(!p.rtt_ms(3));
}

int main() {
  TestWire();
  TestRoundTrip();
  TestLossAndTimeouts();
  TestDuplicateAndOutOfOrder();
  TestAggregates();
  TestMedianAndStaleness();
  TestReset();
  TestGarbage();
  TestSlotRange();
  TestFourPeersKeepIndependentState();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
