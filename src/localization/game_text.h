#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmnt::localization {

enum class TextBank : uint8_t { kUi, kError, kWait, kSubtitle };

// One translated string of a bank: its rank, the CRC-32 of the English text as UTF-8 (a row that no longer matches is
// not applied), the string's name for reading, and the text. `keep_english` is a row written as `=`.
struct GameTextRow {
  TextBank bank = TextBank::kUi;
  uint32_t rank = 0;
  uint32_t english_crc = 0;
  std::string name;
  std::string text;
  bool keep_english = false;
};

struct GameTextTable {
  std::vector<GameTextRow> rows;
  std::vector<std::string> errors;
};

// A bank rebuilt for the language pool the game reads: every string's offset, in characters, and the pool. An empty
// optional is a string the bank does not hold.
struct LocalizedBank {
  std::u16string pool;
  std::vector<std::optional<uint32_t>> offsets;
  uint32_t replaced = 0;
  uint32_t kept = 0;
  uint32_t stale = 0;
};

uint32_t Crc32(std::string_view bytes);

// Decodes a text column: `\r`, `\n`, `\t` and `\\`. False on any other escape.
bool DecodeText(std::string_view escaped, std::string& out);

// Rows are `bank<TAB>rank<TAB>name<TAB>crc32 hex<TAB>text`, one per LF-terminated line; blank lines are ignored.
GameTextTable ParseGameTextTable(std::string_view tsv);

std::u16string Utf8ToUtf16(std::string_view utf8);
std::string Utf16ToUtf8(std::u16string_view utf16);

// The bank a table belongs to, from the id of its first string. The ids are shared by every language of a bank.
std::optional<TextBank> BankForFirstId(uint32_t first_id);

// `english` holds one entry per rank; every matching row's string is replaced and all others are copied unchanged.
LocalizedBank BuildLocalizedBank(TextBank bank, const std::vector<std::optional<std::u16string>>& english,
                                 const std::vector<GameTextRow>& rows);

// The embedded Português Brasil rows (parsed once).
const GameTextTable& PortugueseBrazilGameText();

}  // namespace tmnt::localization
