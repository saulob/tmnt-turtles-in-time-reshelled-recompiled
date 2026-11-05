#pragma once

#include <filesystem>

namespace tmnt {

// Relative to the executable's folder, never to the working directory.
inline std::filesystem::path HeaderArtPath(const std::filesystem::path& executable_folder) {
  return executable_folder / "assets" / "launcher" / "header-art.png";
}

}  // namespace tmnt
