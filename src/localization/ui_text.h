#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tmnt {

// The text of the active language for an English UI string. English text with no translation is returned as is, so a
// missing entry never shows an empty or foreign string. The result outlives the call.
const char* Tr(const char* english);
std::string Tr(const std::string& english);

// For a message built at run time (a name or a number inside known text): the known phrases are replaced, longest
// first and on word boundaries, and everything else is kept.
std::string TrMessage(const std::string& english);

namespace localization {

struct UiTextRow {
  std::string english;
  std::string text;
};

struct UiTextTable {
  std::vector<UiTextRow> rows;
  std::vector<std::string> errors;
};

// Rows are `english<TAB>text`, with the escapes \n, \t and \\ in both columns.
UiTextTable ParseUiTextTable(std::string_view tsv);

const UiTextTable& PortugueseBrazilUiText();

}  // namespace localization
}  // namespace tmnt
