#include "port_version.h"

namespace tmnt {

const std::string& PortVersion() {
  static const std::string version = std::to_string(TMNT_VERSION_MAJOR) + "." + std::to_string(TMNT_VERSION_MINOR) +
                                     "." + std::to_string(TMNT_VERSION_PATCH);
  return version;
}

}  // namespace tmnt
