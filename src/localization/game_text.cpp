#include "localization/game_text.h"

#include <array>
#include <charconv>
#include <map>

namespace tmnt::localization {

extern const char kPtBrGameTextTsv[];

namespace {

constexpr std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
    }
    table[i] = value;
  }
  return table;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  constexpr size_t kLastColumn = 4;
  std::vector<std::string_view> fields;
  size_t start = 0;
  for (;;) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos || fields.size() == kLastColumn) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

std::optional<TextBank> ParseBank(std::string_view name) {
  if (name == "ui") return TextBank::kUi;
  if (name == "error") return TextBank::kError;
  if (name == "wait") return TextBank::kWait;
  if (name == "subtitle") return TextBank::kSubtitle;
  return std::nullopt;
}

}  // namespace

uint32_t Crc32(std::string_view bytes) {
  static constexpr std::array<uint32_t, 256> table = MakeCrcTable();
  uint32_t crc = 0xFFFFFFFFu;
  for (const char c : bytes) {
    crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

bool DecodeText(std::string_view escaped, std::string& out) {
  out.clear();
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] != '\\') {
      out.push_back(escaped[i]);
      continue;
    }
    if (++i >= escaped.size()) return false;
    switch (escaped[i]) {
      case '\\':
        out.push_back('\\');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return false;
    }
  }
  return true;
}

GameTextTable ParseGameTextTable(std::string_view tsv) {
  GameTextTable table;
  size_t start = 0;
  uint32_t number = 0;
  while (start < tsv.size()) {
    size_t end = tsv.find('\n', start);
    if (end == std::string_view::npos) end = tsv.size();
    const std::string_view line = tsv.substr(start, end - start);
    start = end + 1;
    ++number;
    if (line.empty()) continue;
    const std::vector<std::string_view> fields = SplitTabs(line);
    const auto fail = [&](const char* why) {
      table.errors.push_back("row " + std::to_string(number) + ": " + why);
    };
    if (fields.size() != 5) {
      fail("expected bank, rank, name, crc and text");
      continue;
    }
    GameTextRow row;
    const std::optional<TextBank> bank = ParseBank(fields[0]);
    if (!bank) {
      fail("unknown bank");
      continue;
    }
    row.bank = *bank;
    const auto [rank_end, rank_ec] = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), row.rank);
    if (rank_ec != std::errc() || rank_end != fields[1].data() + fields[1].size()) {
      fail("bad rank");
      continue;
    }
    row.name = std::string(fields[2]);
    const auto [crc_end, crc_ec] =
        std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), row.english_crc, 16);
    if (crc_ec != std::errc() || crc_end != fields[3].data() + fields[3].size()) {
      fail("bad crc");
      continue;
    }
    if (fields[4] == "=") {
      row.keep_english = true;
    } else if (!DecodeText(fields[4], row.text)) {
      fail("bad escape in text");
      continue;
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

std::u16string Utf8ToUtf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    const size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    uint32_t code = length == 1 ? c : c & (0xFF >> (length + 1));
    for (size_t k = 1; k < length && i + k < utf8.size(); ++k) code = (code << 6) | (uint8_t(utf8[i + k]) & 0x3F);
    i += length;
    if (code >= 0x10000) {
      code -= 0x10000;
      out.push_back(char16_t(0xD800 + (code >> 10)));
      out.push_back(char16_t(0xDC00 + (code & 0x3FF)));
    } else {
      out.push_back(char16_t(code));
    }
  }
  return out;
}

std::string Utf16ToUtf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size());
  for (size_t i = 0; i < utf16.size(); ++i) {
    uint32_t code = utf16[i];
    if (code >= 0xD800 && code < 0xDC00 && i + 1 < utf16.size()) {
      code = 0x10000 + ((code - 0xD800) << 10) + (uint32_t(utf16[++i]) - 0xDC00);
    }
    if (code < 0x80) {
      out.push_back(char(code));
    } else if (code < 0x800) {
      out.push_back(char(0xC0 | (code >> 6)));
      out.push_back(char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(char(0xE0 | (code >> 12)));
      out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(char(0x80 | (code & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (code >> 18)));
      out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(char(0x80 | (code & 0x3F)));
    }
  }
  return out;
}

std::optional<TextBank> BankForFirstId(uint32_t first_id) {
  switch (first_id) {
    case 0x50402C48:
      return TextBank::kUi;
    case 0x504027B0:
      return TextBank::kError;
    case 0x5040459F:
      return TextBank::kWait;
    case 0x50402E44:
      return TextBank::kSubtitle;
    default:
      return std::nullopt;
  }
}

LocalizedBank BuildLocalizedBank(TextBank bank, const std::vector<std::optional<std::u16string>>& english,
                                 const std::vector<GameTextRow>& rows) {
  std::map<uint32_t, const GameTextRow*> by_rank;
  for (const GameTextRow& row : rows) {
    if (row.bank == bank) by_rank.emplace(row.rank, &row);
  }
  LocalizedBank result;
  result.offsets.reserve(english.size());
  for (size_t rank = 0; rank < english.size(); ++rank) {
    if (!english[rank]) {
      result.offsets.emplace_back();
      continue;
    }
    result.offsets.emplace_back(uint32_t(result.pool.size()));
    const auto found = by_rank.find(uint32_t(rank));
    if (found == by_rank.end()) {
      result.pool += *english[rank];
    } else if (found->second->english_crc != Crc32(Utf16ToUtf8(*english[rank]))) {
      ++result.stale;
      result.pool += *english[rank];
    } else if (found->second->keep_english) {
      ++result.kept;
      result.pool += *english[rank];
    } else {
      ++result.replaced;
      result.pool += Utf8ToUtf16(found->second->text);
    }
    result.pool.push_back(u'\0');
  }
  return result;
}

const GameTextTable& PortugueseBrazilGameText() {
  static const GameTextTable table = ParseGameTextTable(kPtBrGameTextTsv);
  return table;
}

}  // namespace tmnt::localization
