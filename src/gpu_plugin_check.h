#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <string>
#include <string_view>

namespace tmnt {

// Empty when a GPU plugin is selected. Otherwise says why the shipped configuration did not select one.
inline std::string MissingGpuPluginMessage(std::string_view plugin, const std::filesystem::path& config) {
  if (!plugin.empty()) {
    return {};
  }
  const std::string name = config.filename().string();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(config, ec)) {
    return "The game's " + name + " was not found next to the program, so no graphics plugin was selected. "
           "Extract the whole game folder again.";
  }
  std::ifstream in(config, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.find("gpu_plugin") == std::string::npos) {
    return name + " does not select a graphics plugin. Extract the whole game folder again.";
  }
  return name + " could not be applied (it is unreadable or contains an invalid value), so no graphics plugin "
         "was selected. Extract the whole game folder again.";
}

}  // namespace tmnt
