#include "online/peer_ping.h"

#include <climits>
#include <cmath>

namespace tmnt::online {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'P', 'N', 'G'};
constexpr uint8_t kKindPing = 1;
constexpr uint8_t kKindPong = 2;
constexpr size_t kAnsweredMemory = 64;

void PutU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

uint32_t GetU32(const uint8_t* in) {
  return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

std::vector<uint8_t> Encode(uint8_t kind, uint32_t seq, uint32_t time) {
  std::vector<uint8_t> out(PeerPing::kWireSize);
  std::copy(std::begin(kMagic), std::end(kMagic), out.begin());
  out[4] = kind;
  PutU32(&out[5], seq);
  PutU32(&out[9], time);
  return out;
}

}  // namespace

bool PeerPing::LooksLikePing(const uint8_t* data, size_t size) {
  if (data == nullptr || size != kWireSize) return false;
  if (!std::equal(std::begin(kMagic), std::end(kMagic), data)) return false;
  return data[4] == kKindPing || data[4] == kKindPong;
}

std::optional<std::vector<uint8_t>> PeerPing::Due(int slot, uint64_t now_ms) {
  Slot* s = Pick(slots_, slot);
  if (!s) return std::nullopt;
  latest_now_ = std::max(latest_now_, now_ms);

  // A clock that went backwards never expires anything.
  for (auto it = s->pending.begin(); it != s->pending.end();) {
    if (now_ms > it->sent_ms && now_ms - it->sent_ms > kTimeoutMs) {
      ++s->stats.timeouts;
      it = s->pending.erase(it);
    } else {
      ++it;
    }
  }

  const bool due = !s->sent_any || now_ms < s->last_sent_ms || now_ms - s->last_sent_ms >= kIntervalMs;
  if (!due) {
    s->stats.pending = static_cast<uint32_t>(s->pending.size());
    return std::nullopt;
  }

  const uint32_t seq = s->next_seq++;
  s->sent_any = true;
  s->last_sent_ms = now_ms;
  s->pending.push_back({seq, now_ms});
  ++s->stats.sent;
  s->stats.pending = static_cast<uint32_t>(s->pending.size());
  return Encode(kKindPing, seq, static_cast<uint32_t>(now_ms));
}

bool PeerPing::Take(int slot, const uint8_t* data, size_t size, uint64_t now_ms, std::vector<uint8_t>& reply) {
  Slot* s = Pick(slots_, slot);
  if (!s || !LooksLikePing(data, size)) return false;
  latest_now_ = std::max(latest_now_, now_ms);

  const uint32_t seq = GetU32(data + 5);
  const uint32_t time = GetU32(data + 9);
  if (data[4] == kKindPing) {
    reply = Encode(kKindPong, seq, time);
    return true;
  }

  auto it = std::find_if(s->pending.begin(), s->pending.end(), [&](const Outstanding& o) { return o.seq == seq; });
  if (it == s->pending.end()) {
    if (std::find(s->answered.begin(), s->answered.end(), seq) != s->answered.end()) ++s->stats.duplicates;
    return true;
  }
  if (static_cast<uint32_t>(it->sent_ms) != time) return true;  // not the ping we sent under this seq

  const uint64_t elapsed = now_ms >= it->sent_ms ? now_ms - it->sent_ms : 0;
  const int rtt = static_cast<int>(std::min<uint64_t>(elapsed, INT_MAX));
  s->pending.erase(it);

  PingStats& st = s->stats;
  st.pending = static_cast<uint32_t>(s->pending.size());
  ++st.received;
  if (s->has_highest && seq < s->highest) {
    ++st.out_of_order;
  } else {
    s->has_highest = true;
    s->highest = seq;
  }
  s->answered.push_back(seq);
  if (s->answered.size() > kAnsweredMemory) s->answered.pop_front();

  st.last_ms = rtt;
  st.min_ms = st.min_ms < 0 ? rtt : std::min(st.min_ms, rtt);
  st.max_ms = std::max(st.max_ms, rtt);
  st.avg_ms += (rtt - st.avg_ms) / static_cast<double>(st.received);
  if (s->has_prev) st.jitter_ms += (std::abs(rtt - s->prev_rtt) - st.jitter_ms) / 16.0;
  s->has_prev = true;
  s->prev_rtt = rtt;

  s->samples.push_back(rtt);
  if (s->samples.size() > kMedianSamples) s->samples.pop_front();
  s->has_answer = true;
  s->last_answer_ms = now_ms;
  return true;
}

void PeerPing::Reset(int slot) {
  if (Slot* s = Pick(slots_, slot)) *s = Slot{};
}

PingStats PeerPing::stats(int slot) const {
  const Slot* s = Pick(slots_, slot);
  return s ? s->stats : PingStats{};
}

std::optional<int> PeerPing::rtt_ms(int slot) const {
  const Slot* s = Pick(slots_, slot);
  if (!s || s->samples.empty() || !s->has_answer) return std::nullopt;
  if (latest_now_ > s->last_answer_ms && latest_now_ - s->last_answer_ms > kStaleMs) return std::nullopt;
  std::vector<int> sorted(s->samples.begin(), s->samples.end());
  std::sort(sorted.begin(), sorted.end());
  return sorted[sorted.size() / 2];
}

}  // namespace tmnt::online
