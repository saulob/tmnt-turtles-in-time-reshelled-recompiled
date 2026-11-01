#pragma once

#include <cstdint>
#include <mutex>

namespace tmnt {

// Ownership of the game's single search slot: only the newest request may write its result, and only once.
class SearchTickets {
 public:
  uint32_t Begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = next_++;
    return current_;
  }

  bool Owns(uint32_t ticket) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticket == current_;
  }

  bool Complete(uint32_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticket != current_) return false;
    current_ = 0;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  uint32_t current_ = 0;
  uint32_t next_ = 1;
};

}  // namespace tmnt
