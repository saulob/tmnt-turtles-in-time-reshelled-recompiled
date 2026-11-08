// SNES Gameplay: the shared rules, the ROM extractor, the data file, the import status, the start decision, the
// launcher page and the TMNT_r wiring. Headless; the real-ROM checks run only when game/snes holds the ROMs.
// Run: tmnt_snes_gameplay_tests

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack/snes_rom.h"
#include "snes_gameplay/gameplay_data.h"
#include "snes_gameplay/online_rules.h"
#include "snes_gameplay/rules.h"
#include "snes_gameplay/runtime_start.h"

namespace fs = std::filesystem;
using namespace tmnt;
using namespace tmnt::snes;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                   \
  } while (0)

const fs::path kRoot = TMNT_ROOT_DIR;

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
  const std::string text = ReadText(path);
  return std::vector<uint8_t>(text.begin(), text.end());
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  const size_t b = a == std::string::npos ? std::string::npos : text.find(to, a + from.size());
  return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
}

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / (std::string("tmnt_snes_gameplay_") + name);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct RealRom {
  const char* region;
  const char* file;
  VideoStandard video;
};

const RealRom kRealRoms[] = {
    {"USA", "Teenage Mutant Ninja Turtles IV - Turtles in Time (USA).sfc", VideoStandard::kNtsc},
    {"Europe", "Teenage Mutant Hero Turtles IV - Turtles in Time (Europe).sfc", VideoStandard::kPal},
    {"Australia", "Teenage Mutant Ninja Turtles IV - Turtles in Time (Australia).sfc", VideoStandard::kPal},
};

fs::path RealRomPath(const RealRom& rom) { return kRoot / "game" / "snes" / rom.file; }

RuntimeData SnesData() {
  RuntimeData data;
  data.frame_rate = 60.0;
  data.facing = {Facing::kRight, Facing::kRight, Facing::kKeep, Facing::kLeft,
                 Facing::kLeft,  Facing::kLeft,  Facing::kKeep, Facing::kRight};
  data.dash_hold_frames = 32;
  data.throw_hold_frames = 6;
  return data;
}

GameplayData SampleData() {
  GameplayData data;
  data.facing = SnesData().facing;
  data.lane_tolerance = 8;
  data.grab_range = 44;
  data.attack_search_range = 64;
  data.back_attack_range = {56, 56, 56, 64};
  data.throw_hold_frames = 6;
  data.dash_hold_frames = 32;
  data.dash_tap_frames = 16;
  data.dash_auto_default = true;
  data.run_speed = 3 << 16;
  data.walk_speed = {0x18000, 0x18000, 0x14000, 0x24000};
  data.dash_attack_speed = 0x5C000;
  data.dash_attack_frames = 17;
  return data;
}

const std::string kSha(64, 'a');

void TestDirections() {
  std::printf("rules: stick to SNES direction, facing keeps on pure vertical\n");
  CHECK(SnesDirection(0, 0) == kNoDirection);
  CHECK(SnesDirection(1, 0) == 0);
  CHECK(SnesDirection(0.7f, -0.7f) == 1);
  CHECK(SnesDirection(0, -1) == 2);
  CHECK(SnesDirection(-0.7f, -0.7f) == 3);
  CHECK(SnesDirection(-1, 0) == 4);
  CHECK(SnesDirection(-0.7f, 0.7f) == 5);
  CHECK(SnesDirection(0, 1) == 6);
  CHECK(SnesDirection(0.7f, 0.7f) == 7);
  CHECK(SnesDirection(0.3f, 1.0f) == 6);   // 17 degrees off vertical is still up
  CHECK(SnesDirection(0.5f, 1.0f) == 7);   // 27 degrees reads as a diagonal
  const RuntimeData data = SnesData();
  CHECK(NextFacing(kRight, 6, data) == kRight && NextFacing(kLeft, 6, data) == kLeft);
  CHECK(NextFacing(kRight, 2, data) == kRight && NextFacing(kLeft, 2, data) == kLeft);
  CHECK(NextFacing(kRight, 5, data) == kLeft && NextFacing(kLeft, 7, data) == kRight);
  CHECK(NextFacing(kLeft, 0, data) == kRight && NextFacing(kRight, 4, data) == kLeft);
  CHECK(NextFacing(kLeft, kNoDirection, data) == kLeft);
  // Up slightly left keeps the right facing, where Re-Shelled's 95 degree rule turns left.
  CHECK(NextFacing(kRight, SnesDirection(-0.17f, 1.0f), data) == kRight);
}

void TestDash() {
  std::printf("rules: dash after the SNES hold, per controller, back to walking\n");
  const RuntimeData data = SnesData();
  const double dt = 1.0 / 60.0;
  StickState a;
  int frames = 0;
  while (!PushStick(a, 0, dt, data) && frames < 100) ++frames;
  CHECK(frames + 1 == 32);
  CHECK(a.dashing && PushStick(a, 0, dt, data));
  CHECK(!PushStick(a, 7, dt, data) && !a.dashing);  // a diagonal leaves the dash
  CHECK(!PushStick(a, 0, dt, data));                // and the count starts again
  StickState vertical;
  for (int i = 0; i < 200; ++i) CHECK(!PushStick(vertical, 6, dt, data));
  StickState b;
  for (int i = 0; i < 31; ++i) PushStick(b, 4, dt, data);
  StickState c;
  CHECK(!PushStick(c, 4, dt, data));  // one controller's hold never moves another's
  CHECK(PushStick(b, 4, dt, data) && !c.dashing);
  ReleaseStick(b);
  CHECK(!b.dashing && b.direction == kNoDirection && b.held == 0.0);
  HoldStick(a, 0, dt);
  CHECK(!a.dashing);
  RuntimeData pal = data;
  pal.frame_rate = kPalFrameRate;
  StickState p;
  frames = 0;
  while (!PushStick(p, 0, dt, pal) && frames < 100) ++frames;
  CHECK(frames + 1 == 39);  // 32 PAL frames take 0.64 s
}

void TestGrab() {
  std::printf("rules: throw to screen only for a held horizontal push toward the facing side\n");
  const RuntimeData data = SnesData();
  const double dt = 1.0 / 60.0;
  StickState s;
  CHECK(ChooseGrab(s, kRight, data) == GrabChoice::kSlam);  // neutral slams
  for (int i = 0; i < 6; ++i) HoldStick(s, 0, dt);
  CHECK(ChooseGrab(s, kRight, data) == GrabChoice::kThrowToScreen);
  CHECK(ChooseGrab(s, kRight, data) == ChooseGrab(s, kRight, data));
  CHECK(ChooseGrab(s, kLeft, data) == GrabChoice::kSlam);  // pushing away from the facing side
  StickState early;
  for (int i = 0; i < 5; ++i) HoldStick(early, 0, dt);
  CHECK(ChooseGrab(early, kRight, data) == GrabChoice::kSlam);
  StickState turned;
  for (int i = 0; i < 10; ++i) HoldStick(turned, 7, dt);
  HoldStick(turned, 0, dt);
  CHECK(ChooseGrab(turned, kRight, data) == GrabChoice::kSlam);  // just changed from a diagonal
  HoldStick(turned, 0, dt);
  CHECK(ChooseGrab(turned, kRight, data) == GrabChoice::kThrowToScreen);
  StickState up;
  for (int i = 0; i < 20; ++i) HoldStick(up, 6, dt);
  CHECK(ChooseGrab(up, kRight, data) == GrabChoice::kSlam);
  StickState left;
  for (int i = 0; i < 20; ++i) HoldStick(left, 4, dt);
  CHECK(ChooseGrab(left, kLeft, data) == GrabChoice::kThrowToScreen);
  CHECK(ChooseGrab(s, kRight, data) == GrabChoice::kThrowToScreen);  // the other player's stick is its own
}

void TestGate() {
  std::printf("rules: mechanic, data and online gate\n");
  CHECK(MechanicActive(true, true, false, false));
  CHECK(!MechanicActive(false, true, false, false));
  CHECK(!MechanicActive(true, false, false, false));
  CHECK(!MechanicActive(true, true, true, false));
  CHECK(MechanicActive(false, true, true, true));
  CHECK(!MechanicActive(true, false, true, true));
  const RuntimeData data = SnesData();
  const std::string text = EncodeRuntimeData(data);
  CHECK(text == "v1 60.0000 RRKLLLKR 32 6");
  const auto decoded = DecodeRuntimeData(text);
  CHECK(decoded && decoded->facing == data.facing && decoded->dash_hold_frames == 32 && decoded->throw_hold_frames == 6);
  for (const char* bad : {"", "v2 60.0000 RRKLLLKR 32 6", "v1 60.0000 RRKLLLK 32 6", "v1 60.0000 RRKLLLKX 32 6",
                          "v1 10.0 RRKLLLKR 32 6", "v1 60.0000 RRKLLLKR 0 6", "v1 60.0000 RRKLLLKR 32 6 1",
                          "v1 abc RRKLLLKR 32 6"}) {
    CHECK(!DecodeRuntimeData(bad));
  }
}

void TestFile() {
  std::printf("data file: round trip, versions, checksum, source identity\n");
  const GameplayData data = SampleData();
  std::vector<uint8_t> bytes = SerializeGameplay(data, kSha);
  GameplayData parsed;
  std::string sha;
  CHECK(ParseGameplay(bytes, parsed, sha) == GameplayFileError::kOk && parsed == data && sha == kSha);
  std::vector<uint8_t> other = bytes;
  other[12] = uint8_t(kGameplayExtractorVersion + 1);
  CHECK(ParseGameplay(other, parsed, sha) == GameplayFileError::kOtherVersion);
  other = bytes;
  other[8] = uint8_t(kGameplayFormatVersion + 1);
  CHECK(ParseGameplay(other, parsed, sha) == GameplayFileError::kOtherVersion);
  other = bytes;
  other[90] ^= 1;
  CHECK(ParseGameplay(other, parsed, sha) == GameplayFileError::kChecksum);
  other = bytes;
  other.pop_back();
  CHECK(ParseGameplay(other, parsed, sha) == GameplayFileError::kMalformed);
  CHECK(ParseGameplay({}, parsed, sha) == GameplayFileError::kMalformed);
  CHECK(bytes.size() < 256);
}

void TestExtractor() {
  std::printf("extractor: real ROMs give one gameplay set, PAL differs only in frame rate; others fail\n");
  std::vector<uint8_t> junk(classic::kRomSize, 0xEA);
  CHECK(!ExtractGameplay(junk).ok);
  int found = 0;
  GameplayData first;
  for (const RealRom& real : kRealRoms) {
    if (!fs::exists(RealRomPath(real))) continue;
    const std::vector<uint8_t> file = ReadBytes(RealRomPath(real));
    std::span<const uint8_t> rom;
    CHECK(classic::NormalizeRom(file, rom) == classic::RomStatus::kOk);
    const ExtractResult result = ExtractGameplay(rom);
    CHECK(result.ok);
    if (!result.ok) {
      std::printf("    %s: %s\n", real.region, result.detail.c_str());
      continue;
    }
    const GameplayData& d = result.data;
    CHECK(d.video == real.video);
    CHECK(d.facing == SnesData().facing);
    CHECK(d.lane_tolerance == 8 && d.grab_range == 44 && d.attack_search_range == 64);
    CHECK(d.back_attack_range == (std::array<uint8_t, 4>{56, 56, 56, 64}));
    CHECK(d.throw_hold_frames == 6 && d.dash_hold_frames == 32 && d.dash_tap_frames == 16 && d.dash_auto_default);
    CHECK(d.run_speed == 3 << 16);
    CHECK(d.walk_speed == (std::array<int32_t, 4>{0x18000, 0x18000, 0x14000, 0x24000}));
    CHECK(d.dash_attack_speed == 0x5C000 && d.dash_attack_frames == 17);
    GameplayData normalized = d;
    normalized.video = VideoStandard::kNtsc;
    if (found++ == 0) {
      first = normalized;
    } else {
      CHECK(normalized == first);
    }
  }
  if (found == 0) {
    std::printf("    no SNES ROMs in game/snes: real-ROM checks skipped\n");
  }
}

void WriteManifest(const fs::path& root, uint32_t importer_version, const std::string& sha) {
  const fs::path folder = classic::SoundtrackFolder(root);
  const std::string text = ReadText(folder / classic::kManifestFile);
  classic::ClassicSoundtrackManifest manifest;
  CHECK(classic::ParseManifest(text, manifest));
  manifest.importer_version = importer_version;
  manifest.source_rom_sha256 = sha;
  std::ofstream(folder / classic::kManifestFile, std::ios::binary) << classic::SerializeManifest(manifest);
}

void TestStatus() {
  std::printf("status: no import, Ready from one import, old import, other ROM, remove\n");
  TempDir dir("status");
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kNotImported);
  const RuntimeStart none = PrepareRuntimeStart(QueryGameplayStatus(dir.path));
  CHECK(none.data.empty() && !none.warning && none.message == "SNES Gameplay: unavailable, SNES ROM not imported");

  const RealRom* real = nullptr;
  for (const RealRom& r : kRealRoms) {
    if (fs::exists(RealRomPath(r))) {
      real = &r;
      break;
    }
  }
  if (!real) {
    std::printf("    no SNES ROMs in game/snes: import checks skipped\n");
    return;
  }
  const classic::ImportResult imported = classic::ImportSoundtrack(RealRomPath(*real), dir.path);
  CHECK(imported.ok && imported.gameplay);
  CHECK(fs::exists(classic::SoundtrackFolder(dir.path) / kGameplayFile));
  CHECK(classic::QuerySoundtrackStatus(dir.path).status == classic::SoundtrackStatus::kReady);
  const GameplayStatusResult ready = QueryGameplayStatus(dir.path);
  CHECK(ready.status == GameplayStatus::kReady && ready.region == real->region);
  const RuntimeStart start = PrepareRuntimeStart(ready);
  CHECK(!start.warning && Has(start.message, "SNES Gameplay: Ready, source "));
  const auto runtime = DecodeRuntimeData(start.data);
  CHECK(runtime && runtime->dash_hold_frames == 32 && runtime->facing == SnesData().facing);

  const std::string sha = classic::QuerySoundtrackStatus(dir.path).manifest.source_rom_sha256;
  WriteManifest(dir.path, classic::kImporterVersion, std::string(64, 'b'));
  CHECK(classic::QuerySoundtrackStatus(dir.path).status == classic::SoundtrackStatus::kReady);
  GameplayStatusResult stale = QueryGameplayStatus(dir.path);
  CHECK(stale.status == GameplayStatus::kReimportRequired && Has(stale.detail, "different ROM"));
  CHECK(PrepareRuntimeStart(stale).data.empty());
  WriteManifest(dir.path, classic::kImporterVersion, sha);
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kReady);

  const fs::path file = classic::SoundtrackFolder(dir.path) / kGameplayFile;
  const std::vector<uint8_t> saved = ReadBytes(file);
  std::vector<uint8_t> damaged = saved;
  damaged[40] ^= 1;
  std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(damaged.data()), std::streamsize(damaged.size()));
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kReimportRequired);
  fs::remove(file);
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kUnsupportedRom);
  WriteManifest(dir.path, 2, sha);
  const GameplayStatusResult old = QueryGameplayStatus(dir.path);
  CHECK(old.status == GameplayStatus::kReimportRequired && Has(old.detail, "before SNES Gameplay"));
  CHECK(classic::QuerySoundtrackStatus(dir.path).status == classic::SoundtrackStatus::kReady);

  CHECK(classic::ImportSoundtrack(RealRomPath(*real), dir.path).ok);
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kReady);
  std::string error;
  CHECK(classic::RemoveSoundtrack(dir.path, error));
  CHECK(!fs::exists(classic::SoundtrackFolder(dir.path)));
  CHECK(QueryGameplayStatus(dir.path).status == GameplayStatus::kNotImported);
  CHECK(PrepareRuntimeStart(QueryGameplayStatus(dir.path)).data.empty());
}

void TestLauncher() {
  std::printf("launcher: SNES page after Gameplay, soundtrack rows moved, gameplay rows and defaults\n");
  const std::string launcher = ReadText(kRoot / "src" / "launcher.cpp");
  const size_t play = launcher.find("{\"Play\", ");
  const size_t display = launcher.find("{\"Display\", ");
  const size_t graphics = launcher.find("{\"Graphics\", ");
  const size_t gameplay = launcher.find("{\"Gameplay\", ");
  const size_t snes_page = launcher.find("{\"SNES\", \"Features based on the original SNES version\", [this](trg::Ui& ui) { PageSnes(ui); }},");
  const size_t cheats = launcher.find("{\"Cheats\", ");
  const size_t online = launcher.find("{\"Online\", ");
  const size_t achievements = launcher.find("{\"Achievements\", ");
  const size_t controls = launcher.find("{\"Controls\", ");
  const size_t about = launcher.find("{\"About\", ");
  CHECK(snes_page != std::string::npos);
  CHECK(play < display && display < graphics && graphics < gameplay && gameplay < snes_page && snes_page < cheats &&
        cheats < online && online < achievements && achievements < controls && controls < about);
  CHECK(Has(launcher, "c.pages.insert(c.pages.begin() + 1,"));

  const std::string gameplay_page = Between(launcher, "void PageGameplay(trg::Ui& ui) {", "\n  }");
  CHECK(!gameplay_page.empty() && !Has(gameplay_page, "Classic") && !Has(gameplay_page, "SNES"));
  const std::string snes = Between(launcher, "void PageSnes(trg::Ui& ui) {", "\n  }");
  const size_t rom = snes.find("ui.Heading(\"SNES ROM\");");
  const size_t audio = snes.find("ui.Heading(\"AUDIO\");");
  const size_t game = snes.find("ui.Heading(\"GAMEPLAY\");");
  CHECK(rom < snes.find("SnesRomRows(ui, status);") && snes.find("SnesRomRows(ui, status);") < audio &&
        audio < snes.find("ClassicSoundtrackRows(ui, status);") && snes.find("ClassicSoundtrackRows(ui, status);") < game &&
        game < snes.find("SnesGameplayRows(ui);") && snes.find("SnesGameplayRows(ui);") != std::string::npos);
  CHECK(Has(snes, "ui.Help(\"Imported SNES ROM used for classic audio and optional gameplay mechanics\");"));
  for (const char* row : {"ui.Info(\"SNES ROM\"", "ui.Row(\"Import SNES soundtrack\"", "ui.ConfirmRow(\"Remove SNES soundtrack\"",
                          "ui.Toggle(\"Classic Soundtrack\"", "BrowseAndImportClassic();", "RemoveClassicSoundtrack();",
                          "PersistClassicToggle(settings_.GetBool(kClassicSoundtrackSetting, false), status);"}) {
    CHECK(Has(launcher, row));
  }
  const std::string rows = Between(launcher, "void SnesGameplayRows(trg::Ui& ui) {", "\n  }");
  for (const char* key : {"snes::kDirectionalCombatSetting,", "snes::kThrowControlSetting, true,", "snes::kDashSetting, true,"}) {
    CHECK(Has(rows, key));
  }
  CHECK(Has(rows, "ImGui::BeginDisabled(!ready);") && !Has(rows, "kSnesGameplaySetting") && !Has(launcher, "\"SNES Gameplay\""));
  CHECK(Has(launcher, "RefreshSnesGameplay();\n    PersistClassicToggle(false, classic_->status());") ||
        Has(launcher, "RefreshSnesGameplay();\r\n    PersistClassicToggle(false, classic_->status());"));

  const std::string settings = ReadText(kRoot / "src" / "settings.cpp");
  CHECK(!Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_gameplay,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_directional_combat, true,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_throw_control, true,"));
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_snes_dash, true,"));
  for (const char* key : {"snes::kDirectionalCombatSetting,", "snes::kThrowControlSetting,", "snes::kDashSetting,"}) {
    CHECK(Has(settings, key));
  }
  CHECK(!Has(settings, "snes::kSnesGameplaySetting"));
  CHECK(!Has(settings, "kRuntimeDataSetting"));
  CHECK(std::string(kDirectionalCombatSetting) == "tmnt_snes_directional_combat" &&
        std::string(kThrowControlSetting) == "tmnt_snes_throw_control" && std::string(kDashSetting) == "tmnt_snes_dash");

  const std::string pt = ReadText(kRoot / "src" / "localization" / "pt_br_ui_text.cpp");
  for (const char* row : {"SNES\tSNES", "Features based on the original SNES version\tRecursos baseados na vers\xC3\xA3o original de SNES",
                          "Directional Combat\tCombate direcional",
                          "SNES Throw Control\tControle de arremesso SNES", "SNES Dash\tDash SNES", "AUDIO\t", "GAMEPLAY\t",
                          "Imported SNES ROM used for classic audio and optional gameplay mechanics\t",
                          "Use SNES-style combat facing and attack direction\t", "Use predictable SNES-style throw selection\t",
                          "Use movement and dash behavior based on the SNES version\t"}) {
    CHECK(Has(pt, row));
  }
}

void TestWiring() {
  std::printf("wiring: start before the guest, hooks in TMNT_r, online and board fall back to stock\n");
  const std::string cmake = ReadText(kRoot / "CMakeLists.txt");
  const std::string core = Between(cmake, "set(TMNT_CLASSIC_SOUNDTRACK_CORE", ")");
  CHECK(Has(core, "src/snes_gameplay/gameplay_data.cpp") && Has(core, "src/snes_gameplay/runtime_start.cpp"));
  CHECK(Has(Between(cmake, "set(TMNT_SOURCES", ")"), "src/snes_gameplay_runtime.cpp"));
  const std::string dll = Between(cmake, "target_sources(tmnt_turtles_in_time_reshelled_TMNT_r PRIVATE", ")");
  CHECK(Has(dll, "src/snes_gameplay_tmnt_r.cpp") && Has(dll, "src/cheats_tmnt_r.cpp"));
  CHECK(Has(cmake, "add_executable(tmnt_snes_gameplay_tests EXCLUDE_FROM_ALL tests/snes_gameplay_tests.cpp"));

  const std::string app = ReadText(kRoot / "src" / "tmnt_turtles_in_time_reshelled_app.h");
  const size_t start = app.find("tmnt::StartSnesGameplay(user_data_root());");
  CHECK(start != std::string::npos && start < app.find("ReXApp::LaunchModule();", start));

  const std::string hooks = ReadText(kRoot / "src" / "snes_gameplay_tmnt_r.cpp");
  for (const char* hook : {"REX_HOOK_RAW(sub_8754B540)", "REX_HOOK_RAW(sub_8754B318)", "REX_HOOK_RAW(sub_8754B430)",
                           "REX_HOOK_RAW(sub_87679280)", "REX_HOOK_RAW(sub_8767AE70)", "REX_HOOK_RAW(sub_8767CA70)",
                           "REX_HOOK_RAW(sub_8767C730)", "REX_HOOK_RAW(sub_8768BB80)", "REX_HOOK_RAW(sub_87683B20)"}) {
    CHECK(Has(hooks, hook));
  }
  CHECK(Has(hooks, "const bool online = (wanted || locked_on) && ready && Online(ctx, base);") &&
        Has(hooks, "return snes::MechanicActive(wanted, ready, online, locked_on && own_turtle);"));
  CHECK(Has(hooks, "REXLOG_INFO(\"SNES Gameplay: online match, using the features locked for it\");"));
  CHECK(Has(hooks, "Active(ctx, base, snes::Feature::kDash)") && Has(hooks, "Active(ctx, base, snes::Feature::kThrowControl)") &&
        Has(hooks, "Active(ctx, base, snes::Feature::kDirectionalCombat, ControllerFor(vars) != nullptr)"));
  CHECK(Has(hooks, "OnBoard(base, controller->vars)") && Has(hooks, "OnBoard(base, vars)"));
  CHECK(!Has(hooks, "DEBUG_SAULO"));
  const std::string cheats = ReadText(kRoot / "src" / "cheats_tmnt_r.cpp");
  for (const char* hook : {"sub_8754B540", "sub_8754B318", "sub_8754B430", "sub_8767CA70", "sub_8767C730", "sub_8768BB80",
                           "sub_87683B20", "sub_87679280", "sub_8767AE70"}) {
    CHECK(!Has(cheats, hook));
  }
  const std::string runtime = ReadText(kRoot / "src" / "snes_gameplay_runtime.cpp");
  CHECK(Has(runtime, "rex::cvar::SetFlagByName(snes::kRuntimeDataSetting, start.data);"));
}

}  // namespace

int main() {
  TestDirections();
  TestDash();
  TestGrab();
  TestGate();
  TestFile();
  TestExtractor();
  TestStatus();
  TestLauncher();
  TestWiring();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
