#pragma once

#include <string_view>

namespace tmnt::localization {

// The language of the program's own text and of the game text it replaces. Set from the saved setting at start and by
// the launcher while the choice changes.
void SetActiveLanguage(std::string_view key);

bool PortugueseBrazilSelected();

}  // namespace tmnt::localization
