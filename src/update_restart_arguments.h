#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace tmnt {

inline bool IsLauncherVisibilityArgument(std::string_view argument) {
  for (const std::string_view name : {"--tmnt_skip_launcher", "--tmnt_launcher"}) {
    if (argument == name || (argument.starts_with(name) && argument[name.size()] == '=')) {
      return true;
    }
  }
  return false;
}

// The updated program opens the launcher again, whatever the player's launcher setting or the original command line.
inline std::vector<std::string> UpdateRestartArguments(std::vector<std::string> arguments) {
  arguments.erase(std::remove_if(arguments.begin(), arguments.end(), IsLauncherVisibilityArgument), arguments.end());
  arguments.emplace_back("--tmnt_launcher=true");
  return arguments;
}

}  // namespace tmnt
