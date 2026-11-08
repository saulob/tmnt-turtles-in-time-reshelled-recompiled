// Classic Soundtrack in the launcher and at game start: the Gameplay page model, setting rules, ZIP input, runtime
// start decisions, translations and build wiring. Synthetic ROMs only. Run: tmnt_classic_soundtrack_launcher_tests

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "classic_soundtrack/activation.h"
#include "classic_soundtrack/importer.h"
#include "classic_soundtrack/manifest.h"
#include "classic_soundtrack/rom_zip.h"
#include "classic_soundtrack/runtime_start.h"
#include "classic_soundtrack/xma_provider.h"
#include "classic_soundtrack_fixture.h"
#include "classic_soundtrack_page.h"
#include "classic_soundtrack_notice.h"
#include "classic_soundtrack_overlay.h"
#include "corner_overlay.h"
#include "quick_play_unlock.h"
#include "trg/settings.h"
#include "updater/third_party/miniz/miniz.h"

namespace {

namespace fs = std::filesystem;
using namespace tmnt;
using namespace tmnt::classic;
using classic_fixture::Bytes;
using classic_fixture::FingerprintOf;
using classic_fixture::FixChecksum;
using classic_fixture::RomBuilder;

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

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

void WriteFile(const fs::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

class TempDir {
 public:
  explicit TempDir(const char* name) : path_(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

Bytes MakeZip(const std::vector<std::pair<std::string, Bytes>>& entries) {
  mz_zip_archive zip{};
  mz_zip_writer_init_heap(&zip, 0, 0);
  for (const auto& [name, data] : entries) {
    mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_BEST_SPEED);
  }
  void* buffer = nullptr;
  size_t size = 0;
  mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
  Bytes bytes(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + size);
  mz_zip_writer_end(&zip);
  mz_free(buffer);
  return bytes;
}

void Import(const Bytes& rom, const fs::path& root) {
  CHECK(ImportSoundtrackFromBytes(rom, root, {}, FingerprintOf(rom)).ok);
}

void TestRules() {
  std::printf("rules: labels, enable and remove availability, effective setting\n");
  CHECK(std::string(ClassicStatusLabel(ClassicSoundtrackStatus::kNotImported)) == "Not imported");
  CHECK(std::string(ClassicStatusLabel(ClassicSoundtrackStatus::kReady)) == "Ready");
  CHECK(std::string(ClassicStatusLabel(ClassicSoundtrackStatus::kImporting)) == "Importing");
  CHECK(std::string(ClassicStatusLabel(ClassicSoundtrackStatus::kReimportRequired)) == "Re-import required");
  CHECK(std::string(ClassicFailureLabel(ImportFailure::kInvalidRom)) == "Invalid ROM");
  CHECK(std::string(ClassicFailureLabel(ImportFailure::kUnsupportedRom)) == "Unsupported ROM");
  CHECK(std::string(ClassicFailureLabel(ImportFailure::kUnsupportedSoundtrack)) == "Unsupported soundtrack");
  CHECK(std::string(ClassicFailureLabel(ImportFailure::kCorruptSoundtrack)) == "Corrupt soundtrack data");
  CHECK(std::string(ClassicFailureLabel(ImportFailure::kImportFailed)) == "Import failed");
  CHECK(std::string(ClassicImportedMessage("USA")) ==
        "SNES soundtrack imported from the USA ROM. Switch Classic soundtrack On to use it.");
  CHECK(std::string(ClassicImportedMessage("Europe")) ==
        "SNES soundtrack imported from the Europe ROM. Switch Classic soundtrack On to use it.");
  CHECK(std::string(ClassicImportedMessage("Australia")) ==
        "SNES soundtrack imported from the Australia ROM. Switch Classic soundtrack On to use it.");
  CHECK(std::string(ClassicImportedMessage(std::string(kCompatibleRegion))) ==
        "SNES soundtrack imported from a compatible ROM. Switch Classic soundtrack On to use it.");

  CHECK(ClassicStatusFromQuery(SoundtrackStatus::kReady) == ClassicSoundtrackStatus::kReady);
  CHECK(ClassicStatusFromQuery(SoundtrackStatus::kNotImported) == ClassicSoundtrackStatus::kNotImported);
  CHECK(ClassicStatusFromQuery(SoundtrackStatus::kImportFailed) == ClassicSoundtrackStatus::kReimportRequired);
  for (const auto status : {ClassicSoundtrackStatus::kNotImported, ClassicSoundtrackStatus::kImporting,
                            ClassicSoundtrackStatus::kReimportRequired}) {
    CHECK(!ClassicCanEnable(status) && !ClassicEffectiveSetting(true, status));
  }
  CHECK(ClassicCanEnable(ClassicSoundtrackStatus::kReady) && ClassicEffectiveSetting(true, ClassicSoundtrackStatus::kReady));
  CHECK(!ClassicEffectiveSetting(false, ClassicSoundtrackStatus::kReady));
  CHECK(ClassicCanRemove(ClassicSoundtrackStatus::kReady) && ClassicCanRemove(ClassicSoundtrackStatus::kReimportRequired));
  CHECK(!ClassicCanRemove(ClassicSoundtrackStatus::kNotImported) && !ClassicCanRemove(ClassicSoundtrackStatus::kImporting));
}

void TestModel() {
  std::printf("page model: not imported, importing, ready, failures keep the import, re-import, remove\n");
  TempDir dir("tmnt_classic_launcher_model");
  const fs::path root = dir.path() / "user_data";
  ClassicSoundtrackModel model(root);
  CHECK(model.status() == ClassicSoundtrackStatus::kNotImported && model.region().empty());
  CHECK(model.last_failure() == ImportFailure::kNone);

  const Bytes rom = RomBuilder(0).rom();
  model.BeginImport();
  CHECK(model.status() == ClassicSoundtrackStatus::kImporting);
  ImportResult imported = ImportSoundtrackFromBytes(rom, root, {}, FingerprintOf(rom));
  model.FinishImport(imported);
  CHECK(imported.ok && model.status() == ClassicSoundtrackStatus::kReady);
  CHECK(model.region() == kCompatibleRegion && model.last_failure() == ImportFailure::kNone);

  const fs::path junk = dir.path() / "junk.sfc";
  WriteFile(junk, Bytes(1000, 0x55));
  model.BeginImport();
  model.FinishImport(ImportSoundtrack(junk, root));
  CHECK(model.last_failure() == ImportFailure::kInvalidRom);
  CHECK(model.status() == ClassicSoundtrackStatus::kReady);

  Bytes unrelated(kRomSize, 0);
  unrelated[0x7FD5] = 0x20;
  unrelated[0x7FD7] = 10;
  FixChecksum(unrelated);
  const fs::path other_game = dir.path() / "other game.smc";
  WriteFile(other_game, unrelated);
  model.BeginImport();
  model.FinishImport(ImportSoundtrack(other_game, root));
  CHECK(model.last_failure() == ImportFailure::kUnsupportedRom && model.status() == ClassicSoundtrackStatus::kReady);

  const fs::path real_shape = dir.path() / "tmnt.sfc";
  WriteFile(real_shape, rom);
  model.BeginImport();
  model.FinishImport(ImportSoundtrack(real_shape, root));
  CHECK(model.last_failure() == ImportFailure::kUnsupportedSoundtrack && model.status() == ClassicSoundtrackStatus::kReady);

  ImportResult not_started;
  model.BeginImport();
  model.FinishImport(not_started);
  CHECK(model.last_failure() == ImportFailure::kImportFailed);

  const fs::path manifest_path = SoundtrackFolder(root) / kManifestFile;
  ClassicSoundtrackManifest manifest;
  CHECK(ParseManifest(ReadText(manifest_path), manifest));
  manifest.importer_version = 1;
  std::ofstream(manifest_path, std::ios::binary | std::ios::trunc) << SerializeManifest(manifest);
  model.Refresh();
  CHECK(model.status() == ClassicSoundtrackStatus::kReimportRequired && model.region().empty());
  model.BeginImport();
  model.FinishImport(ImportSoundtrackFromBytes(rom, root, {}, FingerprintOf(rom)));
  CHECK(model.status() == ClassicSoundtrackStatus::kReady);
  CHECK(!fs::exists(root / "snes_soundtrack.new") && !fs::exists(root / "snes_soundtrack.old"));

  std::ofstream(SoundtrackFolder(root) / kSoundSetFile, std::ios::binary | std::ios::trunc) << "broken";
  model.Refresh();
  CHECK(model.status() == ClassicSoundtrackStatus::kReimportRequired);

  std::string error;
  CHECK(model.Remove(error) && model.status() == ClassicSoundtrackStatus::kNotImported);
  CHECK(fs::exists(real_shape) && fs::exists(junk) && fs::exists(other_game));
  CHECK(ReadText(real_shape).size() == kRomSize);
  CHECK(!fs::exists(SoundtrackFolder(root)));
}

void TestZip() {
  std::printf("zip input: one ROM is read, anything else is refused\n");
  TempDir dir("tmnt_classic_launcher_zip");
  const Bytes rom = RomBuilder(0).rom();
  Bytes rom_out;
  std::string error;

  const Bytes single = MakeZip({{"readme.txt", Bytes(10, 'a')}, {"Turtles In Time (USA).SFC", rom}});
  CHECK(ExtractRomFromZip(single, kRomSize + kCopierHeaderSize, rom_out, error) && rom_out == rom);
  CHECK(!ExtractRomFromZip(MakeZip({{"a.sfc", rom}, {"b.smc", rom}}), kRomSize + 512, rom_out, error));
  CHECK(Has(error, "more than one"));
  CHECK(!ExtractRomFromZip(MakeZip({{"notes.txt", rom}}), kRomSize + 512, rom_out, error));
  CHECK(Has(error, "no .sfc or .smc"));
  CHECK(!ExtractRomFromZip(MakeZip({{"huge.sfc", Bytes(kRomSize * 2, 0)}}), kRomSize + 512, rom_out, error));
  CHECK(!ExtractRomFromZip(Bytes(64, 0x50), kRomSize + 512, rom_out, error));
  CHECK(!ExtractRomFromZip(Bytes{}, kRomSize + 512, rom_out, error));
  CHECK(IsZipPath(".zip") && IsZipPath(".ZIP") && !IsZipPath(".sfc"));

  const fs::path zip_path = dir.path() / "rom.zip";
  WriteFile(zip_path, single);
  Bytes read;
  std::string detail;
  CHECK(ReadRomFile(zip_path, read, detail) == ImportFailure::kNone && read == rom);
  const fs::path root = dir.path() / "user_data";
  const ImportResult from_zip = ImportSoundtrack(zip_path, root);
  CHECK(!from_zip.ok && from_zip.failure == ImportFailure::kUnsupportedSoundtrack);
  const fs::path bad_zip = dir.path() / "bad.zip";
  WriteFile(bad_zip, Bytes(100, 1));
  CHECK(ImportSoundtrack(bad_zip, root).failure == ImportFailure::kInvalidRom);
  CHECK(fs::exists(zip_path));
}

void TestRuntimeStart() {
  std::printf("game start: Off, Ready, missing, corrupt, outdated; no ROM\n");
  TempDir dir("tmnt_classic_launcher_runtime");
  const fs::path root = dir.path() / "user_data";

  RuntimeStartResult off = PrepareRuntimeStart(false, root);
  CHECK(off.start == RuntimeStart::kOff && off.message == "Classic soundtrack: Off" && off.set.songs.empty());
  RuntimeStartResult missing = PrepareRuntimeStart(true, root);
  CHECK(missing.start == RuntimeStart::kFallback && Has(missing.message, "missing") && missing.set.songs.empty());

  const Bytes rom = RomBuilder(2).rom();
  const fs::path rom_file = dir.path() / "rom.sfc";
  WriteFile(rom_file, rom);
  Import(rom, root);
  fs::remove(rom_file);
  RuntimeStartResult ready = PrepareRuntimeStart(true, root);
  CHECK(ready.start == RuntimeStart::kReady && ready.region == kCompatibleRegion);
  CHECK(ready.message == "Classic soundtrack: Ready, source Compatible");
  CHECK(ready.set.songs.size() == kSongCount && ready.set.packs.size() == kPackCount);
  ClassicSoundtrackProvider provider(std::move(ready.set), nullptr);
  CHECK(provider.active_streams() == 0);
  CHECK(PrepareRuntimeStart(false, root).start == RuntimeStart::kOff);

  const fs::path soundset = SoundtrackFolder(root) / kSoundSetFile;
  std::string bytes = ReadText(soundset);
  bytes[bytes.size() / 2] ^= 1;
  std::ofstream(soundset, std::ios::binary | std::ios::trunc) << bytes;
  RuntimeStartResult corrupt = PrepareRuntimeStart(true, root);
  CHECK(corrupt.start == RuntimeStart::kFallback && Has(corrupt.message, "not usable") && corrupt.set.songs.empty());

  Import(rom, root);
  const fs::path manifest_path = SoundtrackFolder(root) / kManifestFile;
  ClassicSoundtrackManifest manifest;
  ParseManifest(ReadText(manifest_path), manifest);
  manifest.importer_version = 1;
  std::ofstream(manifest_path, std::ios::binary | std::ios::trunc) << SerializeManifest(manifest);
  CHECK(PrepareRuntimeStart(true, root).start == RuntimeStart::kFallback);
}

// The player file the way GameSettings::SaveNow and the game's settings load use it.
struct PlayerFile {
  fs::path path;
  ClassicSettingStore Store() const {
    return {[path = path](const std::string& value) {
              trg::TextFileSettings file(path.string());
              file.Set("tmnt_classic_soundtrack", value);
              return file.Save();
            },
            [path = path] { return trg::TextFileSettings(path.string()).Get("tmnt_classic_soundtrack"); }};
  }
  bool Enabled() const { return trg::TextFileSettings(path.string()).GetBool("tmnt_classic_soundtrack", false); }
  bool HasKey() const { return trg::TextFileSettings(path.string()).Has("tmnt_classic_soundtrack"); }
};

struct ActivationRun {
  bool active = false;
  std::vector<std::string> lines;
  std::vector<std::string> warnings;
  int registrations = 0;
};

std::vector<std::string> g_notes;
void CaptureNote(const char* message) { g_notes.emplace_back(message); }

ActivationRun Activate(ClassicSoundtrackActivation& activation, bool enabled, const fs::path& root, bool accepted = true,
                       RexXmaPcmProvider* callbacks = nullptr) {
  ActivationRun run;
  run.active = activation.Start(
      enabled, root,
      [&](const RexXmaPcmProvider* provided) {
        ++run.registrations;
        if (callbacks) {
          *callbacks = *provided;
        }
        return accepted;
      },
      CaptureNote, CaptureNote, [&](bool warning, const std::string& line) {
        (warning ? run.warnings : run.lines).push_back(line);
      });
  return run;
}

void TestOnReachesTheGame() {
  std::printf("regression: import, switch On, restart with the same user data, the music is active\n");
  TempDir dir("tmnt_classic_launcher_regression");
  const fs::path root = dir.path() / "user_data";
  const PlayerFile player{root / "settings.toml"};
  fs::create_directories(root);
  const Bytes rom = RomBuilder(0).rom();

  ClassicSoundtrackModel model(root);
  CHECK(model.status() == ClassicSoundtrackStatus::kNotImported);
  const ClassicToggleOutcome refused = CommitClassicToggle(true, model.status(), player.Store());
  CHECK(!refused.value && refused.persisted && !player.Enabled());

  model.BeginImport();
  model.FinishImport(ImportSoundtrackFromBytes(rom, root, {}, FingerprintOf(rom)));
  CHECK(model.status() == ClassicSoundtrackStatus::kReady);
  CHECK(!player.Enabled());

  ClassicSoundtrackActivation never_enabled;
  ActivationRun off = Activate(never_enabled, player.Enabled(), root);
  CHECK(!off.active && !never_enabled.active() && never_enabled.ready() && off.registrations == 1);
  CHECK(off.lines.size() == 2 && off.lines[1] == "Classic soundtrack: Off" && off.warnings.empty());
  CHECK(Has(off.lines[0], "Ready, source"));
  CHECK(never_enabled.provider() && !never_enabled.provider()->replacing());

  const ClassicToggleOutcome on = CommitClassicToggle(true, model.status(), player.Store());
  CHECK(on.value && on.persisted);
  CHECK(player.HasKey() && player.Enabled());
  CHECK(Has(ReadText(player.path), "tmnt_classic_soundtrack = true"));

  ClassicSoundtrackModel reopened(root);
  CHECK(reopened.status() == ClassicSoundtrackStatus::kReady && player.Enabled());

  ClassicSoundtrackActivation game;
  g_notes.clear();
  ActivationRun started = Activate(game, player.Enabled(), root);
  CHECK(started.active && game.active() && game.provider() && started.registrations == 1);
  CHECK(started.lines.size() == 2 && started.warnings.empty());
  CHECK(started.lines[0] == "Classic soundtrack: Ready, source Compatible");
  CHECK(started.lines[1] == "Classic soundtrack: enabled, 14 music streams");

  const ClassicToggleOutcome off_again = CommitClassicToggle(false, model.status(), player.Store());
  CHECK(!off_again.value && off_again.persisted && !player.Enabled());
  CHECK(Has(ReadText(player.path), "tmnt_classic_soundtrack = false"));
}

void TestEffectiveState() {
  std::printf("effective state: Off, never enabled, enabled, corrupt, outdated, removed, provider refused\n");
  TempDir dir("tmnt_classic_launcher_effective");
  const fs::path root = dir.path() / "user_data";
  const PlayerFile player{root / "settings.toml"};
  fs::create_directories(root);
  const Bytes rom = RomBuilder(1).rom();
  Import(rom, root);

  ClassicSoundtrackActivation refused;
  ActivationRun declined = Activate(refused, true, root, false);
  CHECK(!declined.active && !refused.active() && !refused.ready() && refused.provider() == nullptr &&
        declined.registrations == 1);
  CHECK(refused.Toggle() == RuntimeToggle::kUnavailable);
  CHECK(declined.warnings.size() == 1 && Has(declined.warnings[0], "did not accept the music provider"));
  CHECK(Has(declined.lines.back(), "Ready, source"));

  ClassicSoundtrackActivation unregistered;
  CHECK(!unregistered.Start(true, root, nullptr, nullptr, nullptr, [](bool, const std::string&) {}));
  CHECK(!unregistered.active());

  const fs::path soundset = SoundtrackFolder(root) / kSoundSetFile;
  const std::string original = ReadText(soundset);
  std::string damaged = original;
  damaged[damaged.size() / 2] ^= 1;
  std::ofstream(soundset, std::ios::binary | std::ios::trunc) << damaged;
  ClassicSoundtrackActivation corrupt;
  ActivationRun corrupt_run = Activate(corrupt, true, root);
  CHECK(!corrupt_run.active && corrupt_run.registrations == 0 && !corrupt.active() && !corrupt.ready());
  CHECK(corrupt_run.warnings.size() == 1 && Has(corrupt_run.warnings[0], "not usable"));
  std::ofstream(soundset, std::ios::binary | std::ios::trunc) << original;

  const fs::path manifest_path = SoundtrackFolder(root) / kManifestFile;
  ClassicSoundtrackManifest manifest;
  ParseManifest(ReadText(manifest_path), manifest);
  manifest.importer_version = 1;
  std::ofstream(manifest_path, std::ios::binary | std::ios::trunc) << SerializeManifest(manifest);
  ClassicSoundtrackActivation outdated;
  CHECK(!Activate(outdated, true, root).active && !outdated.active());

  Import(rom, root);
  ClassicSoundtrackModel model(root);
  CHECK(CommitClassicToggle(true, model.status(), player.Store()).value && player.Enabled());
  std::string error;
  CHECK(model.Remove(error));
  const ClassicToggleOutcome removed = CommitClassicToggle(false, model.status(), player.Store());
  CHECK(!removed.value && removed.persisted && !player.Enabled());
  ClassicSoundtrackActivation after_remove;
  ActivationRun missing = Activate(after_remove, true, root);
  CHECK(!missing.active && missing.warnings.size() == 1 && Has(missing.warnings[0], "missing"));
  ClassicSoundtrackActivation after_remove_off;
  ActivationRun removed_off = Activate(after_remove_off, player.Enabled(), root);
  CHECK(!removed_off.active && removed_off.warnings.empty() && removed_off.registrations == 0);
  CHECK(removed_off.lines.size() == 1 && removed_off.lines[0] == "Classic soundtrack: Off");
  CHECK(after_remove_off.Toggle() == RuntimeToggle::kUnavailable);

  const ClassicSettingStore broken{[](const std::string&) { return false; }, [] { return std::string(); }};
  const ClassicToggleOutcome unsaved = CommitClassicToggle(false, ClassicSoundtrackStatus::kReady, broken);
  CHECK(!unsaved.persisted);
  const ClassicSettingStore wrong_value{[](const std::string&) { return true; }, [] { return std::string("false"); }};
  CHECK(!CommitClassicToggle(true, ClassicSoundtrackStatus::kReady, wrong_value).persisted);
  CHECK(!CommitClassicToggle(true, ClassicSoundtrackStatus::kReady, ClassicSettingStore{}).persisted);
}

void TestMusicDiagnostics() {
  std::printf("diagnostics: unknown large streams are logged once, small ones never\n");
  TempDir dir("tmnt_classic_launcher_diagnostics");
  const fs::path root = dir.path() / "user_data";
  Import(RomBuilder(0).rom(), root);
  ClassicSoundtrackActivation game;
  RexXmaPcmProvider callbacks{};
  g_notes.clear();
  CHECK(Activate(game, true, root, true, &callbacks).active && callbacks.open);

  const std::vector<std::uint8_t> big(40000, 0x5A);
  const std::vector<std::uint8_t> small(4000, 0x5A);
  CHECK(callbacks.open(callbacks.user, 3, small.data(), std::uint32_t(small.size()), 44100, 2) == -1);
  CHECK(g_notes.empty());
  CHECK(callbacks.open(callbacks.user, 3, big.data(), std::uint32_t(big.size()), 44100, 2) == -1);
  CHECK(callbacks.open(callbacks.user, 4, big.data(), std::uint32_t(big.size()), 44100, 2) == -1);
  CHECK(g_notes.size() == 1 && Has(g_notes[0], "DEBUG_SAULO XMA stream not recognised, 40000 bytes, 2 channels"));
  std::vector<std::uint8_t> other = big;
  other[7] = 1;
  CHECK(callbacks.open(callbacks.user, 5, other.data(), std::uint32_t(other.size()), 44100, 2) == -1);
  CHECK(g_notes.size() == 2);
  for (std::uint8_t i = 0; i < 20; ++i) {
    std::vector<std::uint8_t> more = big;
    more[9] = std::uint8_t(i + 10);
    callbacks.open(callbacks.user, 6, more.data(), std::uint32_t(more.size()), 44100, 2);
  }
  CHECK(g_notes.size() == ClassicSoundtrackProvider::kUnmatchedLogLimit);
  CHECK(game.provider()->active_streams() == 0);
}

void TestOverlay() {
  std::printf("overlay: the SNES message sits directly under the FPS counter, Ping moves only while it shows\n");
  CHECK(std::string(SoundtrackNoticeText(SoundtrackNotice::kOn)) == "SNES soundtrack: On");
  CHECK(std::string(SoundtrackNoticeText(SoundtrackNotice::kOff)) == "SNES soundtrack: Off");
  CHECK(std::string(SoundtrackNoticeText(SoundtrackNotice::kUnavailable)) == "SNES soundtrack: Unavailable");
  CHECK(std::string(SoundtrackNoticeText(SoundtrackNotice::kNone)).empty());
  CHECK(FrameCounterSlot() == 0 && ClassicSoundtrackSlot(true) == 1 && ClassicSoundtrackSlot(false) == 0);
  CHECK(PingSlot(true) == 1 && PingSlot(false) == 0);
  CHECK(PingSlot(true, true) == 2 && PingSlot(false, true) == 1 && PingSlot(true, false) == 1);
  for (const float scale : {1.0f, 1.5f, 2.25f}) {
    const float font = 18.0f * scale;
    const float width = 1920.0f * scale;
    const CornerPanel fps = CornerPanelAt(width, font, 9.0f * font, font, FrameCounterSlot());
    const CornerPanel snes = CornerPanelAt(width, font, 15.0f * font, font, ClassicSoundtrackSlot(true));
    const CornerPanel ping = CornerPanelAt(width, font, 6.0f * font, font, PingSlot(true, true));
    const CornerPanel ping_after = CornerPanelAt(width, font, 6.0f * font, font, PingSlot(true, false));
    const CornerPanel top = CornerPanelAt(width, font, 15.0f * font, font, ClassicSoundtrackSlot(false));
    CHECK(snes.x1 == fps.x1 && ping.x1 == fps.x1);
    CHECK(snes.y0 > fps.y1 && snes.y0 - fps.y1 == font * 0.25f);
    CHECK(ping.y0 > snes.y1 && std::abs(ping.y0 - snes.y1 - font * 0.25f) < 0.01f);
    CHECK(top.y0 == fps.y0);
    CHECK(snes.y1 - snes.y0 == fps.y1 - fps.y0);
    CHECK(ping_after.y0 == snes.y0);
  }

  const fs::path root = TMNT_ROOT_DIR;
  const std::string overlay = ReadText(root / "src" / "classic_soundtrack_overlay.cpp");
  CHECK(Has(overlay, "g_notice.Current(ImGui::GetTime())"));
  CHECK(Has(overlay, "DrawCornerText(LocalizedSoundtrackNoticeText(notice), ClassicSoundtrackSlot(counter));"));
  CHECK(!Has(overlay, "IsClassicSoundtrackActive()  ") && Has(overlay, "if (IsClassicSoundtrackActive()) {"));
  size_t draws = 0;
  for (size_t at = overlay.find("DrawCornerText("); at != std::string::npos; at = overlay.find("DrawCornerText(", at + 1)) {
    ++draws;
  }
  CHECK(draws == 1);
  CHECK(Has(ReadText(root / "src" / "ping_display.cpp"), "PingSlot(counter, ClassicSoundtrackNoticeVisible())"));
  CHECK(Has(ReadText(root / "src" / "frame_counter.cpp"), "DrawCornerText(FrameCounterText(stats), FrameCounterSlot());"));
  const std::string app = ReadText(root / "src" / "tmnt_turtles_in_time_reshelled_app.h");
  CHECK(Has(app, "tmnt::CreateClassicSoundtrackOverlay(imgui_drawer());"));
  CHECK(Has(ReadText(root / "src" / "classic_soundtrack_runtime.cpp"), "return Activation().active();"));
  CHECK(Has(ReadText(root / "CMakeLists.txt"), "src/classic_soundtrack_overlay.cpp"));
}

void TestTransientNotices() {
  std::printf("notices: startup 5 s, F9 messages 3 s, one at a time, nothing left behind\n");
  CHECK(kStartupNoticeSeconds == 5.0 && kToggleNoticeSeconds == 3.0);
  SoundtrackNoticeState state;
  CHECK(state.Current(0.0) == SoundtrackNotice::kNone);

  state.Show(SoundtrackNotice::kOn, kStartupNoticeSeconds);
  CHECK(state.Current(100.0) == SoundtrackNotice::kOn);
  CHECK(state.Current(102.5) == SoundtrackNotice::kOn);
  CHECK(state.Current(104.99) == SoundtrackNotice::kOn);
  CHECK(state.Current(105.0) == SoundtrackNotice::kNone);
  CHECK(state.Current(105.1) == SoundtrackNotice::kNone && state.Current(500.0) == SoundtrackNotice::kNone);

  for (const SoundtrackNotice notice : {SoundtrackNotice::kOn, SoundtrackNotice::kOff, SoundtrackNotice::kUnavailable}) {
    state.Show(notice, kToggleNoticeSeconds);
    CHECK(state.Current(200.0) == notice);
    CHECK(state.Current(202.9) == notice);
    CHECK(state.Current(203.0) == SoundtrackNotice::kNone);
  }

  state.Show(SoundtrackNotice::kOff, kToggleNoticeSeconds);
  CHECK(state.Current(300.0) == SoundtrackNotice::kOff);
  state.Show(SoundtrackNotice::kOn, kToggleNoticeSeconds);
  CHECK(state.Current(301.0) == SoundtrackNotice::kOn);
  CHECK(state.Current(303.9) == SoundtrackNotice::kOn);
  CHECK(state.Current(304.0) == SoundtrackNotice::kNone);

  SoundtrackNoticeState early;
  early.Show(SoundtrackNotice::kOn, kStartupNoticeSeconds);
  CHECK(early.Current(1000.0) == SoundtrackNotice::kOn);
  CHECK(early.Current(1004.0) == SoundtrackNotice::kOn);

  CHECK(NoticeForToggle(RuntimeToggle::kOn) == SoundtrackNotice::kOn);
  CHECK(NoticeForToggle(RuntimeToggle::kOff) == SoundtrackNotice::kOff);
  CHECK(NoticeForToggle(RuntimeToggle::kUnavailable) == SoundtrackNotice::kUnavailable);
}

void TestF9Toggle() {
  std::printf("F9: switches this session only, restarts the SNES song, unavailable without an import\n");
  TempDir dir("tmnt_classic_launcher_f9");
  const fs::path root = dir.path() / "user_data";
  const PlayerFile player{root / "settings.toml"};
  fs::create_directories(root);
  Import(RomBuilder(0).rom(), root);
  CHECK(CommitClassicToggle(true, ClassicSoundtrackStatus::kReady, player.Store()).persisted);
  const std::string settings_before = ReadText(player.path);

  for (const bool configured : {true, false}) {
    ClassicSoundtrackActivation game;
    RexXmaPcmProvider callbacks{};
    CHECK(Activate(game, configured, root, true, &callbacks).active == configured);
    CHECK(game.ready() && game.active() == configured);

    CHECK(game.Toggle() == (configured ? RuntimeToggle::kOff : RuntimeToggle::kOn));
    CHECK(game.active() == !configured);
    CHECK(game.Toggle() == (configured ? RuntimeToggle::kOn : RuntimeToggle::kOff));
    CHECK(game.active() == configured);
    for (int press = 0; press < 5; ++press) {
      game.Toggle();
    }
    CHECK(game.active() == !configured);
    game.Toggle();
    CHECK(game.active() == configured);
  }
  CHECK(ReadText(player.path) == settings_before && player.Enabled());

  ClassicSoundtrackActivation empty;
  const fs::path nothing = dir.path() / "nothing";
  ActivationRun unavailable = Activate(empty, true, nothing);
  CHECK(!unavailable.active && !empty.ready());
  CHECK(empty.Toggle() == RuntimeToggle::kUnavailable && empty.Toggle() == RuntimeToggle::kUnavailable);
  CHECK(!empty.active() && !fs::exists(nothing));
  CHECK(NoticeForToggle(empty.Toggle()) == SoundtrackNotice::kUnavailable);

  const std::string overlay = ReadText(fs::path(TMNT_ROOT_DIR) / "src" / "classic_soundtrack_overlay.cpp");
  for (const char* banned : {"SetFlagByName", "SaveNow", "settings_", "REXCVAR_GET(tmnt_classic_soundtrack)", "settings.toml"}) {
    CHECK(!Has(overlay, banned));
  }
  CHECK(Has(overlay, "kClassicSoundtrackKey") && Has(overlay, "ToggleClassicSoundtrack()"));
  CHECK(std::string(kClassicSoundtrackKey) == "F9");
}

void TestQuickPlayUnlock() {
  std::printf("Quick Play debug unlock: RelWithDebInfo only, stage select event only, nothing written\n");
  static_assert(!kQuickPlayUnlockBuild, "the tests build without the RelWithDebInfo definition");
  CHECK(!UnlocksAllQuickPlayStages(kStageSelectInitEvent, kQuickPlayUnlockBuild));
  CHECK(UnlocksAllQuickPlayStages(kStageSelectInitEvent, true));
  CHECK(!UnlocksAllQuickPlayStages(kStageSelectInitEvent + 1, true) && !UnlocksAllQuickPlayStages(127, true));
  CHECK(!UnlocksAllQuickPlayStages(0, true));
  CHECK(kStageSelectInitEvent == 166 && kQuickPlayStageCount == 13);

  const fs::path root = TMNT_ROOT_DIR;
  const std::string cmake = ReadText(root / "CMakeLists.txt");
  CHECK(Has(cmake, "TMNT_DEBUG_QUICKPLAY_UNLOCK=$<IF:$<CONFIG:RelWithDebInfo>,1,0>"));
  std::string hooks = ReadText(root / "src" / "live_hooks_tmnt_r.cpp");
  hooks.erase(std::remove(hooks.begin(), hooks.end(), '\r'), hooks.end());
  CHECK(Has(hooks, "DEBUG_SAULO Quick Play: all stages unlocked for soundtrack testing"));
  CHECK(Has(hooks, "if (UnlocksAllQuickPlayStages(event, kQuickPlayUnlockBuild)) UnlockAllQuickPlayStages(ctx, base, self, track);"));
  const size_t begin = hooks.find("void UnlockAllQuickPlayStages(");
  const size_t end = hooks.find("\n}\n", begin);
  const std::string body = hooks.substr(begin, end - begin);
  CHECK(Has(body, "__imp__sub_875B0ED0(ctx, base);"));
  for (const char* banned : {"Profile", "Achievement", "Save", "Store", "SetFlag", "online", "Online", "STORE"}) {
    CHECK(!Has(body, banned));
  }
  CHECK(Has(ReadText(root / "src" / "quick_play_unlock.h"), "DEBUG_SAULO"));
}

std::set<std::string> PortugueseKeys(const std::string& table, std::vector<std::string>* duplicates) {
  const size_t begin = table.find("R\"PTBR(") + 7;
  const size_t end = table.find(")PTBR\"");
  std::set<std::string> keys;
  std::istringstream lines(table.substr(begin, end - begin));
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    if (!keys.insert(line.substr(0, tab)).second) {
      duplicates->push_back(line.substr(0, tab));
    }
    if (tab + 1 >= line.size()) {
      duplicates->push_back("empty: " + line.substr(0, tab));
    }
  }
  return keys;
}

void TestLocalization() {
  std::printf("translations: every new launcher string has a Portuguese row\n");
  const fs::path root = TMNT_ROOT_DIR;
  std::vector<std::string> problems;
  const std::set<std::string> keys = PortugueseKeys(ReadText(root / "src" / "localization" / "pt_br_ui_text.cpp"), &problems);
  CHECK(problems.empty());
  for (const std::string& problem : problems) {
    std::printf("    %s\n", problem.c_str());
  }
  std::vector<std::string> strings = {
      "Classic soundtrack", "SNES soundtrack", "Import SNES soundtrack", "Remove SNES soundtrack", "Import...",
      "Import again...", "Remove...", "Remove", "Cancel", "Off", "On", "Remove the imported SNES soundtrack?",
      "Select your Teenage Mutant Ninja Turtles IV: Turtles in Time SNES ROM", "SNES ROM", "Importing SNES soundtrack...",
      "Import failed", "Could not remove the imported SNES soundtrack.", "Removed the imported SNES soundtrack.",
      "Language, on-screen information, the classic soundtrack and player names.",
      "Plays the SNES soundtrack instead of the Re-Shelled music. Press F9 during gameplay to switch between them",
      "Deletes the imported music. Your ROM file is not touched.", "Could not save the Classic soundtrack setting.",
      "Your own Teenage Mutant Ninja Turtles IV: Turtles in Time SNES ROM (.sfc, .smc or .zip). The music is imported on this PC; the ROM is not needed afterwards."};
  for (const auto status : {ClassicSoundtrackStatus::kNotImported, ClassicSoundtrackStatus::kReady,
                            ClassicSoundtrackStatus::kImporting, ClassicSoundtrackStatus::kReimportRequired}) {
    strings.push_back(ClassicStatusLabel(status));
  }
  for (const auto failure : {ImportFailure::kInvalidRom, ImportFailure::kUnsupportedRom, ImportFailure::kUnsupportedSoundtrack,
                             ImportFailure::kCorruptSoundtrack, ImportFailure::kImportFailed}) {
    strings.push_back(ClassicFailureLabel(failure));
    strings.push_back(ClassicFailureText(failure));
  }
  for (const char* region : {"USA", "Europe", "Australia", "Compatible"}) {
    strings.push_back(ClassicImportedMessage(region));
  }
  const std::string launcher = ReadText(root / "src" / "launcher.cpp");
  for (const std::string& english : strings) {
    CHECK(keys.count(english) == 1);
    if (!keys.count(english)) {
      std::printf("    missing pt-BR row: %s\n", english.c_str());
    }
  }
  for (const char* literal : {"\"Classic soundtrack\"", "\"SNES soundtrack\"", "\"Import SNES soundtrack\"",
                              "\"Remove SNES soundtrack\"", "\"Remove the imported SNES soundtrack?\""}) {
    CHECK(Has(launcher, literal));
  }
  CHECK(!keys.count("Language, on-screen information and player names."));
}

void TestWiring() {
  std::printf("wiring: game executable, launcher page, patch 0087, docs\n");
  const fs::path root = TMNT_ROOT_DIR;
  const std::string cmake = ReadText(root / "CMakeLists.txt");
  const size_t core = cmake.find("set(TMNT_CLASSIC_SOUNDTRACK_CORE");
  const size_t core_end = cmake.find(")", core);
  const std::string core_list = cmake.substr(core, core_end - core);
  for (const char* file : {"activation.cpp", "spc_player.cpp", "xma_provider.cpp", "soundtrack_stream.cpp", "resampler.cpp", "music_streams.cpp",
                           "runtime_start.cpp", "rom_zip.cpp", "importer.cpp", "soundset.cpp"}) {
    CHECK(Has(core_list, file));
  }
  CHECK(!Has(core_list, "playback_check"));
  const size_t sources = cmake.find("set(TMNT_SOURCES");
  const std::string source_list = cmake.substr(sources, cmake.find(")", sources) - sources);
  CHECK(Has(source_list, "src/classic_soundtrack_runtime.cpp") && Has(source_list, "src/classic_soundtrack_page.cpp"));
  CHECK(!Has(source_list, "playback_check"));
  CHECK(Has(cmake, "list(APPEND TMNT_SOURCES ${TMNT_CLASSIC_SOUNDTRACK_CORE})"));
  CHECK(Has(cmake, "target_link_libraries(tmnt_turtles_in_time_reshelled PRIVATE trg::launcher tmnt_snes_spc"));

  const std::string settings = ReadText(root / "src" / "settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_BOOL(tmnt_classic_soundtrack, false,") && Has(settings, "\"tmnt_classic_soundtrack\","));
  CHECK(!Has(ReadText(root / "config" / "tmnt_turtles_in_time_reshelled.toml"), "tmnt_classic_soundtrack"));
  CHECK(Has(ReadText(root / "src" / "tmnt_turtles_in_time_reshelled_app.h"), "tmnt::StartClassicSoundtrack(user_data_root());"));

  const std::string launcher = ReadText(root / "src" / "launcher.cpp");
  CHECK(Has(launcher, "[this](trg::Ui& ui) { PageGameplay(ui); }"));
  CHECK(Has(launcher, "    ClassicSoundtrackRows(ui);\n") || Has(launcher, "    ClassicSoundtrackRows(ui);\r\n"));
  CHECK(Has(launcher, "PollClassicImport();"));
  CHECK(Has(launcher, "classic_task_.running()) {"));
  CHECK(Has(launcher, "ImGui::BeginDisabled(!usable);"));
  CHECK(Has(launcher, "classic::ImportSoundtrack(rom, root)"));
  CHECK(Has(launcher, "settings_.SaveNow({{kClassicSoundtrackSetting, value}})"));
  CHECK(Has(launcher, "CommitClassicToggle(requested, status, store)"));
  CHECK(Has(launcher, "PersistClassicToggle(settings_.GetBool(kClassicSoundtrackSetting, false), status);"));
  CHECK(Has(launcher, "PersistClassicToggle(false, classic_->status());"));
  const size_t info_row = launcher.find("ui.Info(\"SNES soundtrack\"");
  const size_t import_row = launcher.find("ui.Row(\"Import SNES soundtrack\"");
  const size_t toggle_row = launcher.find("ui.Toggle(\"Classic soundtrack\"");
  const size_t remove_row = launcher.find("ui.ConfirmRow(\"Remove SNES soundtrack\"");
  CHECK(info_row < import_row && import_row < toggle_row && toggle_row < remove_row);
  CHECK(Has(launcher, "\"*.sfc;*.smc;*.zip\""));
  for (const char* banned : {"AnalyzeRom", "KnownRomRegion", "kKnownRoms", "NormalizeRom", "SoundtrackFingerprint"}) {
    CHECK(!Has(launcher, banned));
  }

  const std::string runtime = ReadText(root / "src" / "classic_soundtrack_runtime.cpp");
  CHECK(Has(runtime, "REXCVAR_GET(tmnt_classic_soundtrack), user_data_root,"));
  CHECK(Has(runtime, "rex_audio_set_xma_pcm_provider(callbacks);"));
  CHECK(Has(runtime, "classic::RuntimeToggle ToggleClassicSoundtrack() {"));
  CHECK(Has(runtime, "static auto* activation = new classic::ClassicSoundtrackActivation;"));
  const std::string activation = ReadText(root / "src" / "classic_soundtrack" / "activation.cpp");
  const size_t gate = activation.find("if (start.start != RuntimeStart::kReady) {");
  const size_t registration = activation.find("register_provider(&callbacks_)");
  CHECK(gate != std::string::npos && registration != std::string::npos && gate < registration);
  CHECK(Has(settings, "diagnostics.launcher_gameplay.classic_soundtrack = OnOff(settings.GetBool(\"tmnt_classic_soundtrack\", false));"));
  CHECK(Has(ReadText(root / "src" / "diagnostics.cpp"), "\" | Classic soundtrack=\" + diagnostics.launcher_gameplay.classic_soundtrack"));
  CHECK(Has(settings, "saved->second = value;"));

  const std::string manifest = ReadText(root / "patches" / "rexglue-sdk" / "patches.txt");
  CHECK(Has(manifest, "0087-xma-pcm-provider.patch                            windows,linux  rexruntime"));
  CHECK(fs::exists(root / "patches" / "rexglue-sdk" / "0087-xma-pcm-provider.patch"));

  const std::string docs = ReadText(root / "docs" / "classic-soundtrack.md");
  CHECK(Has(docs, "Import SNES soundtrack") && Has(docs, "Gameplay"));
  CHECK(Has(ReadText(root / "docs" / "launcher.md"), "`tmnt_classic_soundtrack`"));
}

}  // namespace

int main() {
  TestRules();
  TestModel();
  TestZip();
  TestRuntimeStart();
  TestOnReachesTheGame();
  TestEffectiveState();
  TestMusicDiagnostics();
  TestOverlay();
  TestTransientNotices();
  TestF9Toggle();
  TestQuickPlayUnlock();
  TestLocalization();
  TestWiring();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
