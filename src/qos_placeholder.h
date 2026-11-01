#pragma once

#include <cstdint>

namespace tmnt {

inline constexpr uint32_t kInlineQosOffset = 16;

// The title's search object keeps a zeroed XNQOS inline; a real QoS allocation is never at that address.
constexpr bool IsInlineQosPlaceholder(uint32_t qos, uint32_t object) {
  return qos != 0 && object != 0 && qos == object + kInlineQosOffset;
}

}  // namespace tmnt
