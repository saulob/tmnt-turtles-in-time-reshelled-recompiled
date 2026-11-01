#pragma once

#include <cstdint>

namespace tmnt {

// The online gameplay socket takes the first free of these, then any free port. A match advertises the port it got, so
// up to four instances on one PC need nothing else.
inline constexpr uint16_t kGameplayPorts[] = {7001, 7002, 7003, 7004};

}  // namespace tmnt
