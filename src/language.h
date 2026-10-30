#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace tmnt {

// English, French, German, Italian and Spanish are the only languages TMNT.bf ships text for
// (01/00/05/06/07_<Language>_UI_{TextBank,ErrorTextBank,WaitingTextBank,MovieSubtitleBank}). No other
// language's banks exist in the archive. The game has no localized voice for any language: TMNT.bf
// has no audio files at all, and the Bink movies share one audio track across languages, with only
// their subtitles translated per language.
struct LanguageSetting {
  const char* key;          // stored in tmnt_language
  const char* label;        // shown in the launcher
  uint32_t xbox_language;   // the XLanguage value the game reads via user_language at boot
};

inline constexpr char kLanguageSetting[] = "tmnt_language";
inline constexpr char kLanguageDefault[] = "en";
inline constexpr char kLanguageHelp[] =
    "Text language for menus, the HUD and movie subtitles. The game has no spoken dialogue, so this "
    "does not change any voice. Applied when the game starts.";

inline constexpr std::array<LanguageSetting, 5> kLanguages = {{
    {"en", "English", 1},
    {"fr", "French", 4},
    {"de", "German", 3},
    {"it", "Italian", 6},
    {"es", "Spanish", 5},
}};

// An unknown or missing key (an old settings.toml, a hand-edited value) falls back to English.
inline const LanguageSetting& LanguageFor(std::string_view key) {
  for (const LanguageSetting& language : kLanguages) {
    if (key == language.key) {
      return language;
    }
  }
  return kLanguages[0];
}

}  // namespace tmnt
