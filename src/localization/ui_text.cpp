#include "localization/ui_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>

#include "language.h"
#include "localization/active_language.h"
#include "localization/game_text.h"

namespace tmnt {
namespace localization {

extern const char kPtBrUiTextTsv[];

namespace {

std::atomic<bool> g_portuguese_brazil{false};

}  // namespace

void SetActiveLanguage(std::string_view key) { g_portuguese_brazil = key == kPortugueseBrazilKey; }

bool PortugueseBrazilSelected() { return g_portuguese_brazil; }

UiTextTable ParseUiTextTable(std::string_view tsv) {
  UiTextTable table;
  size_t start = 0;
  uint32_t number = 0;
  while (start < tsv.size()) {
    size_t end = tsv.find('\n', start);
    if (end == std::string_view::npos) end = tsv.size();
    const std::string_view line = tsv.substr(start, end - start);
    start = end + 1;
    ++number;
    if (line.empty()) continue;
    const size_t tab = line.find('\t');
    UiTextRow row;
    if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos ||
        !DecodeText(line.substr(0, tab), row.english) || !DecodeText(line.substr(tab + 1), row.text)) {
      table.errors.push_back("row " + std::to_string(number) + ": expected english and text columns");
      continue;
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

const UiTextTable& PortugueseBrazilUiText() {
  static const UiTextTable table = ParseUiTextTable(kPtBrUiTextTsv);
  return table;
}

}  // namespace localization

namespace {

const std::unordered_map<std::string_view, const std::string*>& PortugueseBrazilIndex() {
  static const auto index = [] {
    std::unordered_map<std::string_view, const std::string*> map;
    for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) {
      map.emplace(row.english, &row.text);
    }
    return map;
  }();
  return index;
}

}  // namespace

const char* Tr(const char* english) {
  if (english == nullptr || !localization::PortugueseBrazilSelected()) return english;
  const auto& index = PortugueseBrazilIndex();
  const auto found = index.find(english);
  return found == index.end() ? english : found->second->c_str();
}

std::string Tr(const std::string& english) {
  if (!localization::PortugueseBrazilSelected()) return english;
  const auto& index = PortugueseBrazilIndex();
  const auto found = index.find(english);
  return found == index.end() ? english : *found->second;
}

namespace {

constexpr size_t kMinPhraseBytes = 3;

const std::vector<const localization::UiTextRow*>& PhrasesLongestFirst() {
  static const auto phrases = [] {
    std::vector<const localization::UiTextRow*> rows;
    for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) {
      if (row.english.size() >= kMinPhraseBytes) rows.push_back(&row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->english.size() > b->english.size(); });
    return rows;
  }();
  return phrases;
}

bool IsWordByte(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

bool PhraseAt(std::string_view text, size_t at, std::string_view phrase) {
  if (text.substr(at, phrase.size()) != phrase) return false;
  if (at > 0 && IsWordByte(text[at - 1]) && IsWordByte(phrase.front())) return false;
  const size_t end = at + phrase.size();
  return end == text.size() || !IsWordByte(text[end]) || !IsWordByte(phrase.back());
}

std::string ReplacePhrases(std::string_view english) {
  std::string out;
  out.reserve(english.size());
  for (size_t at = 0; at < english.size();) {
    const localization::UiTextRow* match = nullptr;
    for (const localization::UiTextRow* row : PhrasesLongestFirst()) {
      if (PhraseAt(english, at, row->english)) {
        match = row;
        break;
      }
    }
    if (match) {
      out += match->text;
      at += match->english.size();
    } else {
      out.push_back(english[at++]);
    }
  }
  return out;
}

}  // namespace

std::string TrMessage(const std::string& english) {
  if (!localization::PortugueseBrazilSelected()) return english;
  const auto& index = PortugueseBrazilIndex();
  if (const auto found = index.find(english); found != index.end()) return *found->second;
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> done;
  const std::lock_guard lock(mutex);
  const auto [entry, added] = done.try_emplace(english);
  if (added) entry->second = ReplacePhrases(english);
  return entry->second;
}

}  // namespace tmnt
