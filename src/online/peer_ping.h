#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace tmnt::online {

struct PingStats {
  uint64_t sent = 0, received = 0;
  int last_ms = -1, min_ms = -1, max_ms = -1;
  double avg_ms = 0, jitter_ms = 0;
  uint32_t timeouts = 0, out_of_order = 0, duplicates = 0;
  uint32_t pending = 0;  // pings still waiting for their pong: they are neither answered nor lost yet

  double loss() const {
    const uint64_t settled = sent > pending ? sent - pending : 0;
    const double value = static_cast<double>(timeouts) / static_cast<double>(std::max<uint64_t>(settled, 1));
    return std::clamp(value, 0.0, 1.0);
  }
};

// Round trip per remote participant (slots 1-4) measured with tiny datagrams, 13 bytes each:
// 'T' 'P' 'N' 'G' | kind (1 ping, 2 pong) | seq u32 | sender time u32 (the pinger's own clock, echoed by the pong).
// The pinger keeps its own send time per seq, so the echoed time is only a consistency check.
// Not thread-safe; time is always passed in (milliseconds on one monotonic clock).
class PeerPing {
 public:
  static constexpr uint64_t kIntervalMs = 1000, kTimeoutMs = 3000;
  static constexpr uint64_t kStaleMs = 5000;
  static constexpr size_t kWireSize = 13;
  static constexpr size_t kMedianSamples = 5;

  static bool LooksLikePing(const uint8_t* data, size_t size);

  std::optional<std::vector<uint8_t>> Due(int slot, uint64_t now_ms);
  bool Take(int slot, const uint8_t* data, size_t size, uint64_t now_ms, std::vector<uint8_t>& reply);
  void Reset(int slot);
  PingStats stats(int slot) const;
  // The shown ping. Staleness is judged against the newest time any call has passed in, so the integrator's regular
  // Due() calls are what age it.
  std::optional<int> rtt_ms(int slot) const;

 private:
  struct Outstanding {
    uint32_t seq = 0;
    uint64_t sent_ms = 0;
  };
  struct Slot {
    uint32_t next_seq = 1;
    bool sent_any = false;
    uint64_t last_sent_ms = 0;
    std::vector<Outstanding> pending;
    std::deque<uint32_t> answered;  // recent answered seqs, to tell a duplicate pong from an unknown one
    bool has_highest = false;
    uint32_t highest = 0;
    std::deque<int> samples;
    bool has_answer = false;
    uint64_t last_answer_ms = 0;
    bool has_prev = false;
    int prev_rtt = 0;
    PingStats stats;
  };

  static Slot* Pick(std::array<Slot, 4>& slots, int slot) { return slot >= 1 && slot <= 4 ? &slots[slot - 1] : nullptr; }
  static const Slot* Pick(const std::array<Slot, 4>& slots, int slot) {
    return slot >= 1 && slot <= 4 ? &slots[slot - 1] : nullptr;
  }

  std::array<Slot, 4> slots_;
  uint64_t latest_now_ = 0;
};

}  // namespace tmnt::online
