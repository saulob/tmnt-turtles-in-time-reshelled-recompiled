// Language: the exact supported TMNT language list, the Xbox XLanguage mapping, fallback behavior, and
// that the launcher/settings/startup wiring is in place. Run: tmnt_language_tests

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "language.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string ReadSource(const char* name) {
  return ReadFile(std::string(TMNT_SRC_DIR) + "/" + name);
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

size_t Count(const std::string& text, const char* needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

// The exact matrix proven from TMNT.bf's TextBank/ErrorTextBank/WaitingTextBank/MovieSubtitleBank files
// (00_French, 01_English, 05_German, 06_Italian, 07_Spanish) and the boot-time locale switch in
// default.xex (sub_8228E938, keyed by XLanguage-1). No other language has banks in the archive, and
// the archive has no audio files at all, so none of these five has localized voice either. Portugu\xC3\xAAs Brasil
// is the port's own sixth entry, on the English slot; tests/localization_tests.cpp covers it.
void TestTable() {
  std::printf("the five TMNT-proven languages with their Xbox XLanguage IDs, then the port's pt-BR\n");
  CHECK(tmnt::kLanguages.size() == 6);
  CHECK(std::string(tmnt::kLanguageSetting) == "tmnt_language");
  CHECK(std::string(tmnt::kLanguageDefault) == "en");
  CHECK(tmnt::kLanguageDefault == std::string(tmnt::kLanguages[0].key));

  std::set<std::string> keys;
  std::set<uint32_t> xbox_ids;
  for (const tmnt::LanguageSetting& language : tmnt::kLanguages) {
    keys.insert(language.key);
    xbox_ids.insert(language.xbox_language);
  }
  CHECK(keys.size() == 6);
  CHECK(xbox_ids.size() == 5);

  CHECK(std::string(tmnt::kLanguages[0].key) == "en" && std::string(tmnt::kLanguages[0].label) == "English" &&
        tmnt::kLanguages[0].xbox_language == 1);
  CHECK(std::string(tmnt::kLanguages[1].key) == "fr" && std::string(tmnt::kLanguages[1].label) == "French" &&
        tmnt::kLanguages[1].xbox_language == 4);
  CHECK(std::string(tmnt::kLanguages[2].key) == "de" && std::string(tmnt::kLanguages[2].label) == "German" &&
        tmnt::kLanguages[2].xbox_language == 3);
  CHECK(std::string(tmnt::kLanguages[3].key) == "it" && std::string(tmnt::kLanguages[3].label) == "Italian" &&
        tmnt::kLanguages[3].xbox_language == 6);
  CHECK(std::string(tmnt::kLanguages[4].key) == "es" && std::string(tmnt::kLanguages[4].label) == "Spanish" &&
        tmnt::kLanguages[4].xbox_language == 5);

  CHECK(!Has(tmnt::kLanguageHelp, "voice") || Has(tmnt::kLanguageHelp, "no spoken dialogue"));
}

void TestFallback() {
  std::printf("an unknown or missing language key falls back to English\n");
  CHECK(std::string(tmnt::LanguageFor("en").label) == "English");
  CHECK(std::string(tmnt::LanguageFor("").label) == "English");
  CHECK(std::string(tmnt::LanguageFor("xx").label) == "English");
  CHECK(std::string(tmnt::LanguageFor("english").label) == "English");
  CHECK(std::string(tmnt::LanguageFor("ja").label) == "English");  // not a TMNT-supported language
  CHECK(std::string(tmnt::LanguageFor("pt").label) == "English");
  CHECK(std::string(tmnt::LanguageFor("fr").label) == "French");
  CHECK(std::string(tmnt::LanguageFor("de").label) == "German");
  CHECK(std::string(tmnt::LanguageFor("it").label) == "Italian");
  CHECK(std::string(tmnt::LanguageFor("es").label) == "Spanish");
  CHECK(tmnt::LanguageFor("xx").xbox_language == tmnt::LanguageFor("").xbox_language);
}

void TestSettingsAndLauncherWiring() {
  std::printf("tmnt_language is a registered, applied, round-tripped setting\n");
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_STRING(tmnt_language, \"en\""));
  CHECK(Has(settings, "\"tmnt_language\","));
  CHECK(Has(settings, "#include \"language.h\""));
  CHECK(Has(settings, "SetFlagByName(\"user_language\", std::to_string(language.xbox_language))"));
  CHECK(Has(settings, "ApplyLanguage();"));
  CHECK(Has(settings, "REXLOG_INFO(\"Language: {} (user_language {})\", language.label, language.xbox_language)"));
  CHECK(Has(settings, "diagnostics.launcher_gameplay.language ="));

  const std::string settings_header = ReadSource("settings.h");
  CHECK(Has(settings_header, "void ApplyLanguage();"));

  const std::string app = ReadSource("tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::ApplyLanguage();"));

  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "#include \"language.h\""));
  CHECK(Count(launcher, "ui.Combo(\"Language\",") == 1);
  CHECK(Has(launcher, "kLanguageSetting"));
  const size_t gameplay = launcher.find("void PageGameplay(trg::Ui& ui) {");
  const size_t language_combo = launcher.find("ui.Combo(\"Language\",");
  const size_t frame_rate = launcher.find("\"Frame rate in title\"");
  CHECK(gameplay != std::string::npos && gameplay < language_combo && language_combo < frame_rate);

  const std::string log_policy = ReadSource("log_policy.h");
  CHECK(Has(log_policy, "\"Language:\","));

  const std::string diagnostics_cpp = ReadSource("diagnostics.cpp");
  CHECK(Has(diagnostics_cpp, "\"Launcher Gameplay: Language=\" + diagnostics.launcher_gameplay.language"));

  const std::string diagnostics_header = ReadSource("diagnostics.h");
  CHECK(Has(diagnostics_header, "std::string language;"));
}

void TestNoUnsupportedLanguageExposed() {
  std::printf("the launcher offers no language TMNT has no evidence for\n");
  const std::string launcher = ReadSource("launcher.cpp");
  for (const char* unsupported : {"Japanese", "Korean", "Chinese", "Portuguese", "Dutch", "Russian", "Polish"}) {
    CHECK(!Has(launcher, unsupported));
  }
  CHECK(!Has(ReadSource("language.h"), "Japanese"));
}

}  // namespace

int main() {
  TestTable();
  TestFallback();
  TestSettingsAndLauncherWiring();
  TestNoUnsupportedLanguageExposed();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
