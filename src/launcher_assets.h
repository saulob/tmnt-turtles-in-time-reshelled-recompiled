#pragma once

#include <filesystem>

namespace tmnt {

// Relative to the executable's folder, never to the working directory.
inline std::filesystem::path LauncherImagePath(const std::filesystem::path& executable_folder, const char* file) {
  return executable_folder / "assets" / "launcher" / file;
}

}  // namespace tmnt
