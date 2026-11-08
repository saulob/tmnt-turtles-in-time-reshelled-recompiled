#include "classic_soundtrack/konami_lz.h"

#include <array>

namespace tmnt::classic {
namespace {

constexpr size_t kRingSize = 0x400;
constexpr size_t kRingBias = 0x21;

class Decoder {
 public:
  Decoder(std::span<const std::uint8_t> stream, size_t max_output) : stream_(stream), max_output_(max_output) {}

  LzStatus Run(std::vector<std::uint8_t>& out) {
    size_t at = 2;
    while (at < stream_.size()) {
      const std::uint8_t command = stream_[at++];
      LzStatus status;
      if (command < 0x80) {
        status = Copy(command, at, out);
      } else if (command < 0xA0) {
        status = Literal(command & 0x1F, at, out);
      } else if (command < 0xC0) {
        status = InterleavedZero((command & 0x1F) + 2, at, out);
      } else if (command < 0xE0) {
        status = Fill((command & 0x1F) + 2, at, out);
      } else if (command < 0xFF) {
        status = Emit(0, (command & 0x1F) + 2, out);
      } else {
        if (at >= stream_.size()) {
          return LzStatus::kTruncated;
        }
        status = Emit(0, size_t(stream_[at++]) + 2, out);
      }
      if (status != LzStatus::kOk) {
        return status;
      }
    }
    return LzStatus::kOk;
  }

 private:
  LzStatus Put(std::uint8_t value, std::vector<std::uint8_t>& out) {
    if (out.size() >= max_output_) {
      return LzStatus::kOverflow;
    }
    ring_[ring_at_] = value;
    ring_at_ = (ring_at_ + 1) % kRingSize;
    out.push_back(value);
    return LzStatus::kOk;
  }

  LzStatus Emit(std::uint8_t value, size_t count, std::vector<std::uint8_t>& out) {
    for (size_t i = 0; i < count; ++i) {
      if (const LzStatus status = Put(value, out); status != LzStatus::kOk) {
        return status;
      }
    }
    return LzStatus::kOk;
  }

  LzStatus Copy(std::uint8_t command, size_t& at, std::vector<std::uint8_t>& out) {
    if (at >= stream_.size()) {
      return LzStatus::kTruncated;
    }
    size_t source = ((size_t(command & 3) << 8 | stream_[at++]) + kRingBias) % kRingSize;
    for (size_t i = 0, count = size_t(command >> 2) + 2; i < count; ++i) {
      if (const LzStatus status = Put(ring_[source], out); status != LzStatus::kOk) {
        return status;
      }
      source = (source + 1) % kRingSize;
    }
    return LzStatus::kOk;
  }

  LzStatus Literal(size_t count, size_t& at, std::vector<std::uint8_t>& out) {
    if (count == 0) {
      return LzStatus::kMalformed;
    }
    if (count > stream_.size() - at) {
      return LzStatus::kTruncated;
    }
    for (size_t i = 0; i < count; ++i) {
      if (const LzStatus status = Put(stream_[at++], out); status != LzStatus::kOk) {
        return status;
      }
    }
    return LzStatus::kOk;
  }

  LzStatus InterleavedZero(size_t count, size_t& at, std::vector<std::uint8_t>& out) {
    if (count > stream_.size() - at) {
      return LzStatus::kTruncated;
    }
    for (size_t i = 0; i < count; ++i) {
      if (const LzStatus status = Put(0, out); status != LzStatus::kOk) {
        return status;
      }
      if (const LzStatus status = Put(stream_[at++], out); status != LzStatus::kOk) {
        return status;
      }
    }
    return LzStatus::kOk;
  }

  LzStatus Fill(size_t count, size_t& at, std::vector<std::uint8_t>& out) {
    if (at >= stream_.size()) {
      return LzStatus::kTruncated;
    }
    return Emit(stream_[at++], count, out);
  }

  std::span<const std::uint8_t> stream_;
  size_t max_output_;
  std::array<std::uint8_t, kRingSize> ring_{};
  size_t ring_at_ = 0;
};

}  // namespace

LzResult KonamiLzDecompress(std::span<const std::uint8_t> input, size_t max_output) {
  LzResult result;
  if (input.size() < 2) {
    result.status = LzStatus::kTruncated;
    return result;
  }
  const size_t total = size_t(input[0]) | size_t(input[1]) << 8;
  if (total < 3) {
    result.status = LzStatus::kMalformed;
    return result;
  }
  if (total > input.size()) {
    result.status = LzStatus::kTruncated;
    return result;
  }
  Decoder decoder(input.first(total), max_output);
  result.status = decoder.Run(result.data);
  if (result.status != LzStatus::kOk) {
    result.data.clear();
  }
  return result;
}

}  // namespace tmnt::classic
