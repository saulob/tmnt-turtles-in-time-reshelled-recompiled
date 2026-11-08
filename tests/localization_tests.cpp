// Português Brasil: registration, the live launcher language, the embedded tables and, when the game data is on this
// machine, the tables and the font patch against the real banks, movies and atlas.
// Run: tmnt_localization_tests

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../src/classic_soundtrack_notice.h"
#include "../src/fire_movie_patch.cpp"
#include "../src/language.h"
#include "../src/localization/active_language.h"
#include "../src/localization/font_patch.cpp"
#include "../src/localization/game_text.cpp"
#include "../src/localization/pt_br_game_text.cpp"
#include "../src/localization/pt_br_ui_text.cpp"
#include "../src/localization/ui_text.cpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

void Report(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL %s\n", what.c_str());
  }
}

using namespace tmnt;
using namespace tmnt::localization;
namespace fs = std::filesystem;

constexpr size_t kGuestStringBytes = 199;  // SCR_StringCpy(buffer, text, 200) in FireCommunicator

std::string ReadFile(const std::string& relative) {
  std::ifstream in(fs::path(TMNT_ROOT_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string Lf(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

size_t Find(const std::string& text, std::string_view part) { return text.find(part); }

const GameTextRow* GameRow(TextBank bank, uint32_t rank) {
  for (const GameTextRow& row : PortugueseBrazilGameText().rows) {
    if (row.bank == bank && row.rank == rank) return &row;
  }
  return nullptr;
}

bool ValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    size_t length = 1;
    uint32_t code = c;
    if (c >= 0x80) {
      if ((c & 0xE0) == 0xC0) length = 2, code = c & 0x1F;
      else if ((c & 0xF0) == 0xE0) length = 3, code = c & 0x0F;
      else if ((c & 0xF8) == 0xF0) length = 4, code = c & 0x07;
      else return false;
    }
    if (i + length > s.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      if ((uint8_t(s[i + k]) & 0xC0) != 0x80) return false;
      code = code << 6 | (uint8_t(s[i + k]) & 0x3F);
    }
    const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code < minimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    i += length;
  }
  return true;
}

// printf conversions and the markup the game gives meaning to, as a multiset.
std::map<std::string, int> Tokens(std::string_view text) {
  std::map<std::string, int> tokens;
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == '%' && text[i + 1] != ' ') ++tokens[std::string(text.substr(i, 2))];
  }
  for (const char c : text) {
    if (c == '\r' || c == '\n' || c == '\t') ++tokens[std::string(1, c)];
  }
  return tokens;
}

void TestRegistration() {
  std::printf("languages: pt-BR is registered once and the original five are unchanged\n");
  CHECK(kLanguages.size() == 6);
  CHECK(std::count_if(kLanguages.begin(), kLanguages.end(), [](const LanguageSetting& l) {
          return std::string_view(l.key) == kPortugueseBrazilKey;
        }) == 1);
  CHECK(std::string(kLanguages[0].key) == "en" && std::string(kLanguages[0].label) == "English" && kLanguages[0].xbox_language == 1);
  CHECK(std::string(kLanguages[1].key) == "fr" && std::string(kLanguages[1].label) == "French" && kLanguages[1].xbox_language == 4);
  CHECK(std::string(kLanguages[2].key) == "de" && std::string(kLanguages[2].label) == "German" && kLanguages[2].xbox_language == 3);
  CHECK(std::string(kLanguages[3].key) == "it" && std::string(kLanguages[3].label) == "Italian" && kLanguages[3].xbox_language == 6);
  CHECK(std::string(kLanguages[4].key) == "es" && std::string(kLanguages[4].label) == "Spanish" && kLanguages[4].xbox_language == 5);
  const LanguageSetting& portuguese = LanguageFor("pt-BR");
  CHECK(std::string(portuguese.key) == "pt-BR");
  CHECK(std::string(portuguese.label) == "Portugu\xC3\xAAs Brasil");
  CHECK(portuguese.xbox_language == LanguageFor("en").xbox_language);
  CHECK(std::string(LanguageFor("").key) == "en");
  CHECK(std::string(LanguageFor("pt").key) == "en");
  CHECK(std::string(LanguageFor("PT-br").key) == "en");
}

void TestActiveLanguage() {
  std::printf("active language: pt-BR on, every other language off, missing text stays English\n");
  SetActiveLanguage("en");
  CHECK(!PortugueseBrazilSelected());
  CHECK(std::string(Tr("Display")) == "Display");
  SetActiveLanguage(kPortugueseBrazilKey);
  CHECK(PortugueseBrazilSelected());
  CHECK(std::string(Tr("Display")) == "Tela");
  CHECK(Tr(std::string("Language")) == "Idioma");
  const char* missing = "No row holds this text";
  CHECK(Tr(missing) == missing);
  CHECK(Tr(std::string(missing)) == missing);
  CHECK(Tr(static_cast<const char*>(nullptr)) == nullptr);
  CHECK(!std::string(Tr("Display")).empty());
  for (const char* key : {"en", "fr", "de", "it", "es", "", "unknown"}) {
    SetActiveLanguage(key);
    CHECK(!PortugueseBrazilSelected());
    CHECK(std::string(Tr("Language")) == "Language");
    CHECK(TrMessage("Ana invited you to a party.") == "Ana invited you to a party.");
  }
}

void TestLiveLauncherSwitch() {
  std::printf("launcher: the language changes while it stays open, with nothing saved\n");
  const char* pages[][2] = {{"Display", "Tela"}, {"Graphics", "Gr\xC3\xA1" "ficos"}, {"Gameplay", "Jogabilidade"}, {"Language", "Idioma"}};
  std::string pending = kLanguageDefault;
  const auto frame = [&] { SetActiveLanguage(pending); };
  frame();
  for (const auto& page : pages) CHECK(std::string(Tr(page[0])) == page[0]);
  pending = kPortugueseBrazilKey;
  frame();
  for (const auto& page : pages) CHECK(std::string(Tr(page[0])) == page[1]);
  pending = "en";
  frame();
  for (const auto& page : pages) CHECK(std::string(Tr(page[0])) == page[0]);

  const std::string launcher = Lf(ReadFile("src/launcher.cpp"));
  CHECK(Has(launcher, "trg::SetTranslator(&TranslateLauncherText);"));
  const size_t draw = Find(launcher, "void OnDraw(ImGuiIO& io) override {");
  const size_t set = launcher.find("localization::SetActiveLanguage(settings_.Get(kLanguageSetting, kLanguageDefault));", draw);
  const size_t frame_call = launcher.find("launcher_->Frame()", draw);
  CHECK(draw != std::string::npos && set != std::string::npos && set < frame_call);
  const size_t save = launcher.find("launcher_->Save()", draw);
  CHECK(set < save);

  const std::string patch = ReadFile("patches/trg-launcher/0006-ui-translator.patch");
  for (const char* drawn : {"T(label)", "T(help)", "T(page.name.c_str())", "T(o.label.c_str())", "T(\"Save\")", "T(\"PLAY\")"}) {
    CHECK(Has(patch, drawn));
  }
}

void TestStartupWiring() {
  std::printf("settings and start: tmnt_language persists and selects the English slot under pt-BR\n");
  const std::string settings = Lf(ReadFile("src/settings.cpp"));
  CHECK(Has(settings, "\"tmnt_language\","));
  CHECK(Has(settings, "localization::SetActiveLanguage(language.key);"));
  CHECK(Has(settings, "SetFlagByName(\"user_language\", std::to_string(language.xbox_language))"));
  CHECK(Has(settings, "REXLOG_INFO(\"Language: {} (user_language {})\", language.label, language.xbox_language);"));
  CHECK(Has(settings, "diagnostics.launcher_gameplay.language = LanguageFor("));
}

void TestHooks() {
  std::printf("hooks: game text and font change only under pt-BR\n");
  const std::string text = Lf(ReadFile("src/localization/game_text_hook.cpp"));
  CHECK(Has(text, "REX_HOOK_RAW(sub_82291888)"));
  CHECK(Has(text, "if (table == 0 || !tmnt::localization::PortugueseBrazilSelected()) return;"));
  CHECK(Has(text, "ctx.r3.u64 = table;"));
  const std::string bundle = Lf(ReadFile("src/fire_menu_hook.cpp"));
  const size_t menus = bundle.find("tmnt::PatchMenuBundle(");
  const size_t font = bundle.find("tmnt::localization::AddPortugueseGlyphs(");
  CHECK(menus != std::string::npos && font != std::string::npos && menus < font);
  CHECK(Has(bundle, "if (tmnt::localization::PortugueseBrazilSelected() && tmnt::localization::IsFontBundle(size)) {"));
  const std::string subtitles = Lf(ReadFile("src/localization/subtitle_default_tmnt_r.cpp"));
  CHECK(Has(subtitles, "== tmnt::kPortugueseBrazilKey) ctx.r3.u64 = 1;"));
  const std::string app = Lf(ReadFile("src/tmnt_turtles_in_time_reshelled_app.h"));
  CHECK(Has(app, "return ReXApp::CreateAchievementNotificationDialog();"));
}

void TestBuildAndGate() {
  std::printf("build: every source is in CMake and tools/test.ps1 runs this suite\n");
  const std::string cmake = Lf(ReadFile("CMakeLists.txt"));
  for (const char* source : {"src/localization/font_patch.cpp", "src/localization/game_text.cpp", "src/localization/game_text_hook.cpp",
                             "src/localization/pt_br_game_text.cpp", "src/localization/pt_br_ui_text.cpp", "src/localization/ui_text.cpp",
                             "src/localization/subtitle_default_tmnt_r.cpp", "src/achievement_toast.cpp", "tests/localization_tests.cpp"}) {
    CHECK(Has(cmake, source));
  }
  CHECK(Has(ReadFile("tools/test.ps1"), "tests\\localization_tests.cpp"));
  CHECK(fs::exists(fs::path(TMNT_ROOT_DIR) / "docs/localization-pt-br.md"));
}

void TestGameTable() {
  std::printf("game table: well formed, unique and free of leftover English\n");
  const GameTextTable& table = PortugueseBrazilGameText();
  for (const std::string& error : table.errors) Report(false, "game table " + error);
  CHECK(table.rows.size() == 383);
  std::set<std::pair<int, uint32_t>> ranks;
  std::set<std::pair<int, std::string>> crc_seen;
  int translated = 0;
  // Words a Portuguese sentence never holds; `=` rows are exempt, they keep the English on purpose.
  const char* english_words[] = {" the ", " you ", " your ", " and ", " with ", " press ", " player", "THE ", "YOU ", " is ", " are "};
  for (const GameTextRow& row : table.rows) {
    const std::string where = "row " + row.name + " rank " + std::to_string(row.rank);
    Report(ranks.insert({int(row.bank), row.rank}).second, "duplicate rank: " + where);
    Report(!row.name.empty(), "no name: " + where);
    if (row.keep_english) continue;
    ++translated;
    Report(ValidUtf8(row.text), "invalid UTF-8: " + where);
    Report(!row.text.empty(), "empty text: " + where);
    Report(row.text.size() <= kGuestStringBytes + 40, "far over the guest buffer: " + where);
    const std::u16string wide = Utf8ToUtf16(row.text);
    Report(wide.size() <= kGuestStringBytes, "over the guest buffer: " + where);
    for (const char16_t unit : wide) Report(unit <= 0xFF, "not Latin-1, the game narrows it to a byte: " + where);
    for (const char* word : english_words) Report(!Has(row.text, word), std::string("English word '") + word + "': " + where);
  }
  CHECK(translated == 303);
}

void TestUiTable() {
  std::printf("UI table: well formed, unique, placeholders kept\n");
  const UiTextTable& table = PortugueseBrazilUiText();
  for (const std::string& error : table.errors) Report(false, "UI table " + error);
  CHECK(table.rows.size() > 500);
  std::set<std::string> english;
  for (const UiTextRow& row : table.rows) {
    Report(english.insert(row.english).second, "duplicate row: " + row.english);
    Report(ValidUtf8(row.english) && ValidUtf8(row.text), "invalid UTF-8: " + row.english);
    Report(!row.text.empty(), "empty text: " + row.english);
    std::map<std::string, int> a = Tokens(row.english), b = Tokens(row.text);
    // The game's achievement text ends in CR LF or spaces; the Portuguese row does not.
    a.erase("\r"), a.erase("\n"), b.erase("\r"), b.erase("\n");
    Report(a == b, "placeholders differ: " + row.english);
    const bool spaced = !row.english.empty() && (row.english.back() == ' ' || row.english.front() == ' ');
    if (spaced && Has(row.english, "\r") == false && row.english.size() > 12 && row.english.find(".") == std::string::npos) {
      Report((row.english.back() == ' ') == (row.text.back() == ' ') && (row.english.front() == ' ') == (row.text.front() == ' '),
             "joining space lost: " + row.english);
    }
  }
}

void TestMessages() {
  std::printf("run-time messages: known phrases change, names and numbers stay\n");
  SetActiveLanguage(kPortugueseBrazilKey);
  CHECK(TrMessage("Ana invited you to a party.") == "Ana convidou voc\xC3\xAA para um grupo.");
  CHECK(TrMessage("In your party \xC2\xB7 Connected \xC2\xB7 Direct \xC2\xB7 12 ms") == "No seu grupo \xC2\xB7 Conectado \xC2\xB7 Direto \xC2\xB7 12 ms");
  CHECK(TrMessage("Party 2/4") == "Grupo 2/4");
  CHECK(TrMessage("Online Match \xC2\xB7 Windows \xC2\xB7 v1.2.3") == "Partida Online \xC2\xB7 Windows \xC2\xB7 v1.2.3");
  CHECK(TrMessage("Partying") == "Partying");
  CHECK(TrMessage("Semicolon or Space") == "Semicolon ou Espa\xC3\xA7o");
  CHECK(TrMessage("UDP endpoint available on UDP port 7001. Direct peer connectivity will be verified when joining a match.") ==
        "Endpoint UDP dispon\xC3\xADvel na porta UDP 7001. A conex\xC3\xA3o direta com outros jogadores ser\xC3\xA1 verificada ao entrar em uma partida.");
  SetActiveLanguage("en");
}

void TestSoundtrackNotices() {
  std::printf("classic soundtrack messages: follow the selected language at draw time\n");
  using tmnt::LocalizedSoundtrackNoticeText;
  using tmnt::SoundtrackNotice;
  SetActiveLanguage("en");
  CHECK(std::string(LocalizedSoundtrackNoticeText(SoundtrackNotice::kOn)) == "SNES soundtrack: On");
  SetActiveLanguage(kPortugueseBrazilKey);
  CHECK(std::string(LocalizedSoundtrackNoticeText(SoundtrackNotice::kOn)) == "Trilha sonora SNES: Ligada");
  CHECK(std::string(LocalizedSoundtrackNoticeText(SoundtrackNotice::kNone)).empty());

  // The same notice read after each language switch, as the overlay does every frame.
  SoundtrackNoticeState startup;
  startup.Show(SoundtrackNotice::kOn, kStartupNoticeSeconds);
  CHECK(std::string(LocalizedSoundtrackNoticeText(startup.Current(10.0))) == "Trilha sonora SNES: Ligada");
  SetActiveLanguage("en");
  CHECK(std::string(LocalizedSoundtrackNoticeText(startup.Current(11.0))) == "SNES soundtrack: On");

  const char* description = "Plays the SNES soundtrack instead of the Re-Shelled music";
  CHECK(Has(ReadFile("src/launcher.cpp"), description));
  SetActiveLanguage("en");
  CHECK(std::string(Tr(description)) == description && !Has(Tr(description), "F9"));
  SetActiveLanguage(kPortugueseBrazilKey);
  CHECK(std::string(Tr(description)) == "Toca a trilha sonora do SNES no lugar da m\xC3\xBAsica do Re-Shelled");
  const char* help = "Press F9 while playing to open the SNES menu. Changes made there apply only to the current session.";
  CHECK(Has(ReadFile("src/launcher.cpp"), help));
  CHECK(std::string(Tr(help)) ==
        "Pressione F9 durante o jogo para abrir o menu SNES. As altera\xC3\xA7\xC3\xB5" "es feitas ali valem apenas para a sess\xC3\xA3o atual.");
  SetActiveLanguage("en");
  const std::string docs = ReadFile("docs/classic-soundtrack.md");
  CHECK(Has(docs, "Any supported USA, Europe or Australia ROM works") && Has(docs, "all three give\n   the same soundtrack"));
  CHECK(Has(docs, "does not include the SNES ROM or any music taken from it"));

  const std::string overlay = ReadFile("src/classic_soundtrack_overlay.cpp");
  CHECK(Has(overlay, "LocalizedSoundtrackNoticeText(notice)"));
  CHECK(!Has(overlay, "SNES soundtrack") && !Has(overlay, "Trilha sonora"));
  CHECK(!Has(overlay, "DrawCornerText(SoundtrackNoticeText"));
}

void TestAchievementsAndOnline() {
  std::printf("achievements and online: every row is translated\n");
  SetActiveLanguage(kPortugueseBrazilKey);
  const char* changed[] = {"Heroes in a Half Shell", "Slow as a Turtle", "Staying Alive", "Invincible", "Seen My Bones?", "I Will Survive",
                           "Achievement Unlocked", "%d / %d unlocked", "Unlock progress is saved automatically", "B  Back",
                           "Online play", "Online name", "Your friend code", "Connection test", "Add a friend", "Friends", "Direct",
                           "Connecting", "Connected", "Ready for online play", "Ping display", "Online service", "Invite friends",
                           "Online match invite", "Accept", "Decline", "Update available", "UPDATE NOW", "Settings saved. Starting game...",
                           "PLAY", "Save", "Quit", "Back", "UNLOCKED", "Settings saved.", "Could not save the settings.", "Show this launcher",
                           "Reset settings", "Reset all settings", "Put every setting back to its default?", "Press a key...  (Esc cancels)"};
  for (const char* english : changed) Report(std::string(Tr(english)) != english, std::string("not translated: ") + english);
  CHECK(std::string(Tr("Relay")) == "Relay");
  CHECK(std::string(Tr("Turtle Power")) == "Turtle Power");
  SetActiveLanguage("en");

  const std::string protocol = Lf(ReadFile("src/online/protocol.cpp"));
  CHECK(!Has(protocol, "localization") && !Has(protocol, "Tr("));
  const std::string service = Lf(ReadFile("src/online/online_service.cpp"));
  CHECK(!Has(service, "localization"));
}

void TestBankBuild() {
  std::printf("bank rebuild: stale rows stay English, offsets follow the new text\n");
  const std::string first = "HELLO";
  const std::vector<GameTextRow> rows = {
      {TextBank::kWait, 0, Crc32(first), "a", "Ol\xC3\xA1", false},
      {TextBank::kWait, 1, 0x12345678, "b", "Velho", false},
      {TextBank::kWait, 2, Crc32("KEEP"), "c", "", true},
      {TextBank::kUi, 0, Crc32(first), "d", "Other bank", false},
  };
  const LocalizedBank bank = BuildLocalizedBank(TextBank::kWait, {u"HELLO", u"STALE", u"KEEP", std::nullopt, u"PLAIN"}, rows);
  CHECK(bank.replaced == 1 && bank.stale == 1 && bank.kept == 1);
  CHECK(bank.offsets.size() == 5);
  CHECK(bank.offsets[0] == 0u);
  CHECK(bank.offsets[1] == 4u);
  CHECK(!bank.offsets[3]);
  CHECK(std::u16string(bank.pool.c_str() + *bank.offsets[0]) == u"Olá");
  CHECK(std::u16string(bank.pool.c_str() + *bank.offsets[1]) == u"STALE");
  CHECK(std::u16string(bank.pool.c_str() + *bank.offsets[4]) == u"PLAIN");
  CHECK(BankForFirstId(0x50402C48) == TextBank::kUi);
  CHECK(BankForFirstId(0x504027B0) == TextBank::kError);
  CHECK(BankForFirstId(0x5040459F) == TextBank::kWait);
  CHECK(BankForFirstId(0x50402E44) == TextBank::kSubtitle);
  CHECK(!BankForFirstId(0));
  CHECK(Utf16ToUtf8(Utf8ToUtf16("A\xC3\xA3\xE2\x80\x99")) == "A\xC3\xA3\xE2\x80\x99");
}

void TestBlockCodec() {
  std::printf("DXT5 alpha: a block survives encoding within one level\n");
  uint8_t pixels[16];
  for (int i = 0; i < 16; ++i) pixels[i] = uint8_t(i * 17);
  uint8_t block[8];
  EncodeDxt5Alpha(pixels, block);
  uint8_t decoded[16];
  DecodeDxt5Alpha(block, decoded);
  for (int i = 0; i < 16; ++i) CHECK(std::abs(int(decoded[i]) - int(pixels[i])) <= 19);
  std::memset(pixels, 0, sizeof pixels);
  EncodeDxt5Alpha(pixels, block);
  DecodeDxt5Alpha(block, decoded);
  CHECK(std::all_of(decoded, decoded + 16, [](uint8_t a) { return a == 0; }));
  std::memset(pixels, 200, sizeof pixels);
  EncodeDxt5Alpha(pixels, block);
  DecodeDxt5Alpha(block, decoded);
  CHECK(std::all_of(decoded, decoded + 16, [](uint8_t a) { return a == 200; }));
}

GlyphCell Cell(const std::vector<std::string>& rows) {
  GlyphCell cell{int(rows[0].size()), int(rows.size()), {}};
  for (const std::string& row : rows) {
    for (const char c : row) cell.alpha.push_back(c == '#' ? 255 : 0);
  }
  return cell;
}

void TestReplaceAccent() {
  std::printf("glyph composition: the accent is swapped, the letter is kept\n");
  const GlyphCell circumflex = Cell({"..##..", ".#..#.", "#....#", "......", "######", "#....#", "######", "#....#", "######"});
  const GlyphCell tilde = Cell({".##.#.", "#.##..", "#..#..", "......", "#....#", "##...#", "#.#..#", "#..#.#", "#...##"});
  const auto composed = ReplaceAccent(circumflex, tilde);
  CHECK(composed.has_value());
  if (composed) {
    CHECK(composed->rise == 0);
    CHECK(composed->cell.width == circumflex.width && composed->cell.height == circumflex.height);
    const auto at = [&](int x, int y) { return composed->cell.alpha[size_t(y) * composed->cell.width + x]; };
    CHECK(at(1, 0) == 255 && at(2, 0) == 255 && at(0, 0) == 0);
    CHECK(at(0, 4) == 255 && at(5, 8) == 255);
  }
  CHECK(!ReplaceAccent(Cell({"##", "##"}), tilde));
}

// ---------------------------------------------------------------- real game data

constexpr size_t kBfHeader = 5184, kBfFolder = 12, kBfRecord = 200, kBfPayload = 32, kBundleHeader = 12;

template <typename T>
T Read(const uint8_t* data, size_t at) {
  T value;
  std::memcpy(&value, data + at, sizeof value);
  return value;
}

std::vector<uint8_t> DecompressLzo1x(const uint8_t* s, size_t expected) {
  std::vector<uint8_t> o;
  o.reserve(expected);
  size_t ip = 0;
  const auto extended = [&](size_t base) {
    size_t n = base;
    while (s[ip] == 0) n += 255, ++ip;
    return n + s[ip++];
  };
  int state = 0;
  if (s[0] > 17) {
    const size_t n = size_t(s[0]) - 17;
    o.insert(o.end(), s + 1, s + 1 + n);
    ip = 1 + n;
    state = n < 4 ? int(n) : 4;
  }
  for (;;) {
    const uint8_t t = s[ip++];
    size_t distance, length, trail;
    if (t >= 64) {
      distance = ((t >> 2) & 7) + (size_t(s[ip]) << 3) + 1;
      ++ip;
      length = size_t(t >> 5) + 1;
      trail = t & 3;
    } else if (t >= 32) {
      length = ((t & 31) ? (t & 31) : extended(31)) + 2;
      const size_t d = s[ip] | size_t(s[ip + 1]) << 8;
      ip += 2;
      distance = (d >> 2) + 1;
      trail = d & 3;
    } else if (t >= 16) {
      length = ((t & 7) ? (t & 7) : extended(7)) + 2;
      const size_t d = s[ip] | size_t(s[ip + 1]) << 8;
      ip += 2;
      distance = (size_t(t & 8) << 11) + (d >> 2);
      if (distance == 0) return o;
      distance += 0x4000;
      trail = d & 3;
    } else if (state == 0) {
      const size_t n = (t ? t + 3 : extended(15) + 3);
      o.insert(o.end(), s + ip, s + ip + n);
      ip += n;
      state = 4;
      continue;
    } else {
      distance = (t >> 2) + (size_t(s[ip]) << 2) + (state < 4 ? 1 : 2049);
      ++ip;
      length = state < 4 ? 2 : 3;
      trail = t & 3;
    }
    const size_t from = o.size() - distance;
    for (size_t i = 0; i < length; ++i) o.push_back(o[from + i]);
    o.insert(o.end(), s + ip, s + ip + trail);
    ip += trail;
    state = int(trail);
  }
}

struct Archive {
  std::vector<uint8_t> bytes;
  std::map<std::string, const uint8_t*> records;

  std::vector<uint8_t> Entry(const std::string& name) const {
    const uint8_t* record = records.at(name);
    const uint8_t* payload = bytes.data() + Read<uint32_t>(record, 108);
    const uint32_t compressed = Read<uint32_t>(payload, 0);
    const uint32_t size = Read<uint32_t>(payload, 4);
    if (compressed < size) return DecompressLzo1x(payload + kBfPayload, size);
    return {payload + kBfPayload, payload + kBfPayload + compressed};
  }

  std::vector<uint8_t> Bundle(const std::string& name) const {
    const uint8_t* block = bytes.data() + Read<uint32_t>(records.at(name), 108) + kBfPayload;
    return DecompressLzo1x(block + kBundleHeader, Read<uint32_t>(block, 8));
  }
};

bool LoadArchive(Archive& archive) {
  std::ifstream in(fs::path(TMNT_ROOT_DIR) / "game/extracted/Data/TMNT.bf", std::ios::binary);
  if (!in) return false;
  archive.bytes.assign(std::istreambuf_iterator<char>(in), {});
  const uint32_t count = Read<uint32_t>(archive.bytes.data(), kBfHeader);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* record = archive.bytes.data() + kBfHeader + kBfFolder + kBfRecord * i;
    archive.records[reinterpret_cast<const char*>(record)] = record;
  }
  return true;
}

using Bank = std::vector<std::optional<std::u16string>>;

Bank ParseBank(const std::vector<uint8_t>& bytes) {
  const uint32_t count = Read<uint32_t>(bytes.data(), 4);
  const size_t pool = 12 + 36 * size_t(count);
  Bank bank;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = Read<uint32_t>(bytes.data(), 8 + 36 * size_t(i) + 4);
    if (offset == 0xFFFFFFFF) {
      bank.emplace_back();
      continue;
    }
    std::u16string text;
    for (size_t at = pool + size_t(offset) * 2; at + 2 <= bytes.size(); at += 2) {
      const char16_t unit = char16_t(Read<uint16_t>(bytes.data(), at));
      if (!unit) break;
      text.push_back(unit);
    }
    bank.emplace_back(std::move(text));
  }
  return bank;
}

struct BankFile {
  TextBank bank;
  const char* name;
  uint32_t strings;
  uint32_t first_id;
};

constexpr BankFile kBanks[] = {
    {TextBank::kUi, "UI_TextBank", 258, 0x50402C48},
    {TextBank::kError, "UI_ErrorTextBank", 32, 0x504027B0},
    {TextBank::kWait, "UI_WaitingTextBank", 16, 0x5040459F},
    {TextBank::kSubtitle, "UI_MovieSubtitleBank", 78, 0x50402E44},
};

std::set<uint32_t> MovieFontCodes(const std::vector<uint8_t>& bundle, int* fonts) {
  const std::string_view text(reinterpret_cast<const char*>(bundle.data()), bundle.size());
  const std::string_view signature(kFontSignature, kFontSignatureBytes);
  std::set<uint32_t> common;
  bool first = true;
  for (size_t at = text.find(signature); at != std::string_view::npos; at = text.find(signature, at + 1)) {
    const uint8_t* tag = bundle.data() + at - kFontTagToSignature;
    const size_t count = Read<uint16_t>(tag, kFontGlyphCount);
    const size_t codes = kFontOffsets + Read<uint16_t>(tag, kFontOffsets + count * 2);
    std::set<uint32_t> set;
    uint32_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t code = Read<uint16_t>(tag, codes + i * 2);
      Report(i == 0 || code > previous, "a movie font's code table is no longer sorted");
      previous = code;
      set.insert(code);
    }
    if (first) common = set;
    else std::erase_if(common, [&](uint32_t code) { return !set.count(code); });
    first = false;
    ++*fonts;
  }
  return common;
}

std::set<uint32_t> DescriptorCodes(const std::vector<uint8_t>& bundle) {
  std::set<uint32_t> codes;
  for (size_t i = 0; i < kDescriptorGlyphs; ++i) {
    codes.insert(Read<uint16_t>(bundle.data(), kDescriptorOffset + kDescriptorHeaderBytes + i * kDescriptorRowBytes));
  }
  return codes;
}

// The engine's own text calls, not the banks. None draws for a player: docs/localization-pt-br.md says why for each.
void TestGuestLiterals() {
  const std::string image = ReadFile("game/analysis/tmnt_r.image.bin");
  if (image.empty()) {
    std::printf("SKIP guest literals: game/analysis/tmnt_r.image.bin is not on this machine\n");
    return;
  }
  std::printf("guest literals: every direct engine text call is known\n");
  const std::set<std::string> known = {
      "/F0/ /S25/Me Is Enemy",
      "/F0/!!",
      "/F0//S0.3//C00DDDD//M0/",
      "/F0//S0.3//CFFFFFF//M0/",
      "/F0//S0.3//Fd X=3 Y=3 bgf=1 bgs=32 bgc=aaff3333/= Evil debug menu =",
      "/F0//S0.4/Actor ID:",
      "/F0//S0.4/Boss ",
      "/F0//S0.4/Player",
      "/F0//S0.4/Station ID:",
      "/F0//S0.5//CFFFF00//M0/Game Paused",
      "/F0//S0.5/Are you sure you want to quit?",
      "/F0//S0.5/B:Yes    A:No",
      "/F0//S0.8//C00DDDD//M0/",
      "/F0//S0.8//CFFFFFF//M0/",
      "/F0//S2.0/OMG YOU WON!",
      "/F0//jxc/Players connected: /M30/",
      "/F1/ /Jxcyc/!",
      "/F1//S0.7/",
      "/F1//S1.0/Player /M1/:",
      "/F1//jxc/Press [Start] to continue, [Back] to go back.",
      "/F1//jxc/Waiting for host...",
      "/F3//C11AA11//c11DD11/Teenage /c11DD11/Mutant /c11DD11/Ninja /c11DD11/Turtles",
      "/F3//jxc//CDD3333FF/Ninja /C00AA00FF/Turtles",
  };
  size_t found = 0;
  for (size_t at = image.find("/F"); at != std::string::npos; at = image.find("/F", at + 1)) {
    if (at + 3 >= image.size() || image[at + 3] != '/') continue;
    const size_t end = image.find('\0', at);
    const std::string text = image.substr(at, end - at);
    if (text.size() > 120 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= ' ' && c <= '~'; })) continue;
    if (at > 0 && image[at - 1] != '\0' && image[at - 1] != '/') continue;
    ++found;
    Report(known.count(text) != 0, "a new engine text call: " + text);
  }
  CHECK(found > 0);
  for (const std::string& text : known) Report(Has(image, text), "no longer in the image: " + text);
}

void TestRealGame() {
  Archive archive;
  if (!LoadArchive(archive)) {
    std::printf("SKIP real game data: game/extracted/Data/TMNT.bf is not on this machine\n");
    return;
  }

  std::printf("real banks: every row matches its English source and fits what the game proves\n");
  std::set<uint32_t> used;
  int strings = 0, kept = 0, replaced = 0;
  size_t over_shipped = 0;
  const std::set<std::string> wider = {"btn_select",  "select",    "settings_fx", "mp_onl_create_pl_num",
                                       "turtle_stats_speed", "option_on", "option_off"};
  for (const BankFile& file : kBanks) {
    const std::string suffix = std::string(file.name) + "_360.txl";
    const Bank english = ParseBank(archive.Entry("01_English_" + suffix));
    CHECK(english.size() == file.strings);
    CHECK(Read<uint32_t>(archive.Entry("01_English_" + suffix).data(), 8) == file.first_id);
    const LocalizedBank bank = BuildLocalizedBank(file.bank, english, PortugueseBrazilGameText().rows);
    Report(bank.stale == 0, std::string("stale rows in ") + file.name);
    strings += int(file.strings);
    kept += int(bank.kept);
    replaced += int(bank.replaced);
    for (uint32_t rank = 0; rank < file.strings; ++rank) {
      if (!english[rank]) continue;
      const GameTextRow* row = GameRow(file.bank, rank);
      Report(row != nullptr, std::string("no row for ") + file.name + " rank " + std::to_string(rank));
      if (!row || row->keep_english) continue;
      Report(Tokens(row->text) == Tokens(Utf16ToUtf8(*english[rank])) || row->name.starts_with("loc_buy_now"),
             "markup differs: " + row->name);
      size_t longest = english[rank]->size();
      for (const char* language : {"00_French_", "05_German_", "06_Italian_", "07_Spanish_"}) {
        const Bank other = ParseBank(archive.Entry(language + suffix));
        if (other[rank]) longest = std::max(longest, other[rank]->size());
      }
      if (Utf8ToUtf16(row->text).size() > longest) {
        ++over_shipped;
        Report(wider.count(row->name) != 0, "longer than every shipped language: " + row->name);
      }
    }
    for (uint32_t rank = 0; rank < file.strings; ++rank) {
      if (!english[rank]) continue;
      const GameTextRow* row = GameRow(file.bank, rank);
      for (const char16_t unit : row && !row->keep_english ? Utf8ToUtf16(row->text) : *english[rank]) {
        if (unit > 0x20) used.insert(unit);
      }
    }
  }
  CHECK(strings == 384);
  CHECK(replaced == 303);
  std::printf("   %d strings, %d translated, %d kept on purpose, %zu slightly longer than any shipped language\n", strings, replaced, kept,
              over_shipped);

  std::printf("real font: ã õ Ã Õ are added to the descriptor, every movie and the atlas\n");
  int total_fonts = 0;
  std::map<std::string, std::vector<uint8_t>> patched;
  for (const char* name : {"f88d0000.bin", "f88d0001.bin", "f88d0002.bin"}) {
    std::vector<uint8_t> bundle = archive.Bundle(name);
    const std::vector<uint8_t> original = bundle;
    CHECK(IsFontBundle(bundle.size()));
    if (IsMenuBundle(bundle.size())) {
      const BundlePatchResult movies = PatchMenuBundle(bundle.data(), bundle.size());
      CHECK(movies.failed == 0);
    }
    const FontPatchResult result = AddPortugueseGlyphs(bundle.data(), bundle.size());
    CHECK(result.atlases == 1);
    CHECK(result.descriptors == (std::string(name) == "f88d0001.bin" ? 1 : 0));
    CHECK(result.movie_fonts > 0);
    total_fonts += result.movie_fonts;
    CHECK(bundle.size() == original.size());
    CHECK(bundle != original);
    const FontPatchResult again = AddPortugueseGlyphs(bundle.data(), bundle.size());
    CHECK(again.atlases == 0 && again.movie_fonts == 0 && again.descriptors == 0);
    patched[name] = std::move(bundle);
  }
  CHECK(total_fonts == 55);

  const std::vector<uint8_t>& loading = patched["f88d0001.bin"];
  const std::set<uint32_t> descriptor = DescriptorCodes(loading);
  CHECK(descriptor.size() == kDescriptorGlyphs);
  for (const GlyphSwap& swap : kSwaps) {
    CHECK(descriptor.count(swap.added) == 1);
    CHECK(descriptor.count(swap.dropped) == 0);
  }
  for (const uint32_t code : used) {
    Report(descriptor.count(code) != 0, "not in the font descriptor: U+" + std::to_string(code));
  }
  for (const auto& [name, bundle] : patched) {
    int fonts = 0;
    const std::set<uint32_t> movie = MovieFontCodes(bundle, &fonts);
    CHECK(fonts > 0);
    for (const uint32_t code : used) {
      // The legal glyphs missing from the 176-glyph variant never appear in a bank.
      Report(movie.count(code) != 0, name + ": not in every movie font: U+" + std::to_string(code));
    }
    for (const GlyphSwap& swap : kSwaps) CHECK(movie.count(swap.added) == 1 && movie.count(swap.dropped) == 0);
  }

  std::printf("real atlas: the new glyphs hold ink in free cells and the old ones are untouched\n");
  std::vector<uint8_t> untouched = archive.Bundle("f88d0001.bin");
  const uint8_t* before = FindAtlas(untouched);
  std::vector<uint8_t> copy = loading;
  const std::string_view text(reinterpret_cast<const char*>(copy.data()), copy.size());
  const size_t found = text.find(std::string_view(reinterpret_cast<const char*>(kAtlasHeader), kAtlasHeaderBytes));
  CHECK(before != nullptr && found != std::string_view::npos);
  if (before && found != std::string_view::npos) {
    const uint8_t* after = copy.data() + found + kAtlasHeaderBytes;
    size_t changed_blocks = 0;
    for (size_t block = 0; block < kAtlasBytes / kBlockBytes; ++block) {
      if (std::memcmp(before + block * kBlockBytes, after + block * kBlockBytes, kBlockBytes) != 0) {
        ++changed_blocks;
        const int row = int(block / (kAtlasSide / 4)) * 4;
        const int y = kAtlasSide - 1 - row;
        Report(y >= kCellY - 4 && y < kCellY + 52, "a block outside the new cells changed");
      }
    }
    CHECK(changed_blocks > 0);
    for (const GlyphSwap& swap : kSwaps) {
      int ink = 0;
      for (int y = kCellY; y < kCellY + 40; ++y) {
        for (int x = swap.cell_x; x < swap.cell_x + swap.base_rect.width; ++x) ink += ReadAlpha(after, x, y) > kInkAlpha;
      }
      CHECK(ink > 100);
    }
  }

  std::printf("original languages: the stock banks and font stay as shipped without the patch\n");
  const Bank french = ParseBank(archive.Entry("00_French_UI_TextBank_360.txl"));
  const std::string joined = Utf16ToUtf8(*french[0]);
  CHECK(!joined.empty());
  std::vector<uint8_t> wrong(kDescriptorBytes, 0);
  CHECK(AddPortugueseGlyphs(wrong.data(), wrong.size()).atlases == 0);
}

}  // namespace

int main() {
  TestRegistration();
  TestActiveLanguage();
  TestLiveLauncherSwitch();
  TestStartupWiring();
  TestHooks();
  TestBuildAndGate();
  TestGameTable();
  TestUiTable();
  TestMessages();
  TestSoundtrackNotices();
  TestAchievementsAndOnline();
  TestBankBuild();
  TestBlockCodec();
  TestReplaceAccent();
  TestGuestLiterals();
  TestRealGame();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
