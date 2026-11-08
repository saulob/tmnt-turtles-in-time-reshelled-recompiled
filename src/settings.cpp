#include "settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/logging.h>

#include "trg/settings.h"

#include "aspect_ratio.h"
#include "cheats.h"
#include "connection_test_gate.h"
#include "crash_report.h"
#include "diagnostics.h"
#include "game_files.h"
#include "graphics_adapters.h"
#include "input_summary.h"
#include "keybinds.h"
#include "language.h"
#include "localization/active_language.h"
#include "legacy_settings.h"
#include "multisampling.h"
#include "player_names.h"
#include "quick_play_unlock.h"
#include "retired_settings.h"
#include "snes_gameplay/rules.h"
#include "system_info.h"
#include "updater/sha256.h"

#include "platform.h"

REXCVAR_DEFINE_BOOL(tmnt_launcher, true, "TMNT",
                    "Show the launcher before the game starts (hold Shift at start to force it)");
REXCVAR_DEFINE_BOOL(tmnt_skip_launcher, false, "TMNT",
                    "Skip the launcher once (set when the launcher restarts the game)");
REXCVAR_DEFINE_BOOL(tmnt_skip_intro, false, "TMNT", "Start directly at the title screen");
REXCVAR_DEFINE_BOOL(tmnt_classic_soundtrack, false, "TMNT",
                    "In development: replace the music with the imported SNES soundtrack");
REXCVAR_DEFINE_BOOL(tmnt_snes_directional_combat, true, "TMNT",
                    "SNES: SNES combat facing and attack direction; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_snes_throw_control, true, "TMNT", "SNES: predictable SNES throw selection; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_snes_disable_double_jump, false, "TMNT",
                    "SNES: no Re-Shelled double jump; a second jump request in the air is ignored");
REXCVAR_DEFINE_STRING(tmnt_snes_dash, "auto", "TMNT", "SNES: how the dash starts: off, auto (hold) or manual (double tap)");
REXCVAR_DEFINE_BOOL(tmnt_cheat_infinite_health, false, "TMNT",
                    "Cheat: local players' turtles keep their maximum health; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_cheat_infinite_lives, false, "TMNT",
                    "Cheat: a defeated local player keeps the same number of lives; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_turbo_attack, false, "TMNT",
                    "Cheat: holding the attack button repeats the attack during play; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_turbo_strong_attack, false, "TMNT",
                    "Cheat: holding the strong attack button repeats the strong attack during play; not applied online");
REXCVAR_DEFINE_INT32(tmnt_cheat_damage_multiplier, 0, "TMNT",
                     "Cheat: multiplies the damage players deal, 0 Off, 2 or 4; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_cheat_super_armor, false, "TMNT",
                    "Cheat: a hit still deals damage but neither interrupts an attack nor knocks back; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_cheat_spin_attack, false, "TMNT",
                    "Cheat: B starts the Spinning Pizza spin attack during play; not applied online");
REXCVAR_DEFINE_BOOL(tmnt_unlock_all_stages, false, "TMNT",
                    "Quick Play lists every stage; saved progress is not changed");
REXCVAR_DEFINE_BOOL(tmnt_show_fps, true, "TMNT", "Show the frame rate in the window title");
REXCVAR_DEFINE_BOOL(tmnt_show_render_resolution, false, "TMNT",
                    "Show the render resolution in the window title");
REXCVAR_DEFINE_BOOL(tmnt_show_frame_counter, false, "TMNT",
                    "Show the frame counter in the corner (F2 toggles it)");
REXCVAR_DEFINE_BOOL(tmnt_msaa_compatibility, false, "TMNT",
                    "Draw the game's 2x MSAA render targets with 4x MSAA, for GPUs or drivers whose "
                    "native 2x MSAA misrenders (sets native_2x_msaa)");
REXCVAR_DEFINE_STRING(tmnt_aspect_ratio, "16:9", "TMNT",
                      "Aspect ratio: only 16:9 is available, any other value is read as 16:9");
REXCVAR_DEFINE_STRING(tmnt_language, "en", "TMNT",
                      "Text language: en, fr, de, it, es or pt-BR (sets user_language at start)");
REXCVAR_DEFINE_STRING(tmnt_graphics_adapter, "auto", "TMNT",
                      "Graphics card: auto, or the index d3d12_adapter (Windows) or vulkan_device (Linux) takes (applied at start)");
REXCVAR_DEFINE_STRING(tmnt_player_1_name, "Player 1", "TMNT",
                      "Name shown for local player 1; save data is not affected");
REXCVAR_DEFINE_STRING(tmnt_player_2_name, "Player 2", "TMNT",
                      "Name shown for local player 2; save data is not affected");
REXCVAR_DEFINE_STRING(tmnt_player_3_name, "Player 3", "TMNT",
                      "Name shown for local player 3; save data is not affected");
REXCVAR_DEFINE_STRING(tmnt_player_4_name, "Player 4", "TMNT",
                      "Name shown for local player 4; save data is not affected");
REXCVAR_DEFINE_INT32(tmnt_launcher_x, 0, "TMNT", "Launcher window left edge, written by the launcher");
REXCVAR_DEFINE_INT32(tmnt_launcher_y, 0, "TMNT", "Launcher window top edge, written by the launcher");
REXCVAR_DEFINE_INT32(tmnt_launcher_width, 0, "TMNT", "Launcher window restored width, written by the launcher");
REXCVAR_DEFINE_INT32(tmnt_launcher_height, 0, "TMNT",
                     "Launcher window restored height, written by the launcher");
REXCVAR_DEFINE_BOOL(tmnt_launcher_maximized, false, "TMNT", "Launcher window was maximized when closed");
REXCVAR_DEFINE_BOOL(tmnt_launcher_fullscreen, false, "TMNT",
                    "Launcher was left in its own F11 fullscreen, written by the launcher");

namespace tmnt {
namespace {

// Every setting the launcher captures, saves, applies and resets. The keyboard bindings come from
// the launcher's own table so their names live in one place.
std::vector<std::string> GameSettingKeys() {
  std::vector<std::string> keys = {
      "tmnt_launcher",
      "fullscreen",
      "window_width",
      "window_height",
      "monitor",
      kTearingSetting,
      "tmnt_graphics_adapter",
      "tmnt_aspect_ratio",
      "tmnt_language",
      kOnlineEnabledSetting,
      kOnlineNameSetting,
      kOnlinePingSetting,
      kConnectionTestSetting,
      "resolution_scale",
      "swap_post_effect",
      "tmnt_msaa_compatibility",
      "anisotropic_override",
      "tmnt_skip_intro",
      "tmnt_classic_soundtrack",
      snes::kDirectionalCombatSetting,
      snes::kThrowControlSetting,
      snes::kDoubleJumpSetting,
      snes::kDashSetting,
      cheats::kInfiniteHealthSetting,
      cheats::kInfiniteLivesSetting,
      cheats::kTurboAttackSetting,
      cheats::kTurboStrongAttackSetting,
      cheats::kDamageMultiplierSetting,
      cheats::kSuperArmorSetting,
      cheats::kSpinAttackSetting,
      kUnlockAllStagesSetting,
      "tmnt_show_fps",
      "tmnt_show_render_resolution",
      "tmnt_show_frame_counter",
      "detailed_logging",
      "mnk_mode",
      "tmnt_player_1_name",
      "tmnt_player_2_name",
      "tmnt_player_3_name",
      "tmnt_player_4_name",
  };
  for (const KeyboardAction& action : kKeyboardActions) {
    keys.emplace_back(action.key);
  }
  return keys;
}

const std::vector<std::string> kGameSettingKeys = GameSettingKeys();

const std::vector<std::string> kWindowKeys = {
    "window_width",
    "window_height",
    "monitor",
    kTearingSetting,
    "tmnt_graphics_adapter",
};

bool IsWindowKey(std::string_view key) {
  return std::find(kWindowKeys.begin(), kWindowKeys.end(), key) != kWindowKeys.end();
}

// Monitor 0 (Default) is the primary display, as monitor 1 is.
std::string WindowValue(std::string_view key, const std::string& value) {
  if (key == "monitor") {
    return std::to_string(std::max(1, std::atoi(value.c_str())));
  }
  return value;
}

std::string TomlValue(std::string_view key, const std::string& value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
  if (!info || info->type != rex::cvar::FlagType::String) {
    return value;
  }
  return TomlString(value);
}

std::optional<std::string> ConfigValue(const toml::table& table, std::string_view key) {
  const toml::node* node = table.get(key);
  if (!node) {
    return std::nullopt;
  }
  if (auto v = node->value_exact<bool>()) {
    return *v ? "true" : "false";
  }
  if (auto v = node->value_exact<int64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = node->value_exact<double>()) {
    return std::to_string(*v);
  }
  return node->value_exact<std::string>();
}

trg::TextFileSettings OpenPlayerFile(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (!std::filesystem::exists(path, ec)) {
    std::ofstream(path) << "# Turtles in Time Re-Shelled player settings, written by the launcher.\n"
                           "# Loaded after tmnt_turtles_in_time_reshelled.toml. Remove a line to use the shipped value.\n";
  }
  return trg::TextFileSettings(Utf8(path));
}

std::string CvarText(std::string_view name, std::string_view fallback) {
  const std::string value = rex::cvar::GetFlagByName(name);
  return value.empty() ? std::string(fallback) : value;
}

std::string OnOff(bool value) {
  return value ? "On" : "Off";
}

std::string ChoiceLabel(std::initializer_list<std::pair<std::string_view, std::string_view>> choices,
                        std::string_view value, std::string_view fallback) {
  for (const auto& [key, label] : choices) {
    if (key == value) {
      return std::string(label);
    }
  }
  return std::string(fallback);
}

std::string SelectedGraphicsCardLabel(const GameSettings& settings) {
  const std::vector<GraphicsAdapter> adapters = ListGraphicsAdapters();
  const std::optional<int> index = ResolveGraphicsAdapter(settings.Get("tmnt_graphics_adapter", ""), adapters);
  if (!index) {
    return "Unavailable";
  }
  for (const GraphicsAdapter& adapter : adapters) {
    if (adapter.index != *index) {
      continue;
    }
    const auto same_name = std::count_if(adapters.begin(), adapters.end(), [&](const GraphicsAdapter& other) {
      return other.name == adapter.name;
    });
    return same_name > 1 ? adapter.name + " (" + std::to_string(adapter.index) + ")" : adapter.name;
  }
  return "Unavailable";
}

std::string TextureFilteringName(std::string_view value) {
  if (value == "-1") {
    return "Game";
  }
  if (value == "0") {
    return "Off";
  }
  if (value == "2") {
    return "2x";
  }
  if (value == "3") {
    return "4x";
  }
  if (value == "4") {
    return "8x";
  }
  if (value == "5") {
    return "16x";
  }
  return std::string(value);
}

std::string GameStateName(GameFiles state) {
  switch (state) {
    case GameFiles::kReady:
      return "verified";
    case GameFiles::kUnsupported:
      return "unsupported version";
    case GameFiles::kMissing:
      return "missing";
  }
  return "unknown";
}

}  // namespace

// Runs before the file is parsed: ReXGlue rejects a whole TOML file that contains one bad value.
void RepairLegacyPlayerSettings(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::size_t repaired = 0;
  const std::string fixed = MigrateBoolToDashMode(
      RepairBareString(RepairLegacyToggles(text, repaired), kConnectionTestSetting, repaired), snes::kDashSetting, repaired);
  if (repaired == 0) {
    return;
  }

  std::filesystem::path backup = path;
  backup += ".bak";
  std::error_code ec;
  std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    REXLOG_ERROR("Not repairing {}: cannot write the backup", Utf8(path.filename()));
    return;
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary);
    out << fixed;
    if (!out) {
      REXLOG_ERROR("Cannot write the repaired settings to {}", Utf8(temp.filename()));
      return;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    REXLOG_ERROR("Cannot replace {} with the repaired settings", Utf8(path.filename()));
    return;
  }
  REXLOG_INFO("Repaired {} legacy value(s) in {} (original kept as {})", repaired, Utf8(path.filename()),
              Utf8(backup.filename()));
}

namespace {

// A retired setting has no cvar and no launcher control; an old file may still hold its line, which does nothing.
void RemoveRetiredPlayerSettings(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::string kept = text;
  std::string removed;
  for (const std::string_view key : kRetiredPlayerSettings) {
    std::string next = RemoveSettingLines(kept, key);
    if (next != kept) {
      removed += removed.empty() ? "" : ", ";
      removed += key;
      kept = std::move(next);
    }
  }
  if (removed.empty()) {
    return;
  }

  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary);
    out << kept;
    if (!out) {
      REXLOG_ERROR("Cannot write the player settings without {} to {}", removed, Utf8(temp.filename()));
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    REXLOG_ERROR("Cannot replace {} with the player settings without {}", Utf8(path.filename()), removed);
    return;
  }
  REXLOG_INFO("Removed the retired setting(s) {} from {}", removed, Utf8(path.filename()));
}

}  // namespace

void LoadPlayerSettings(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }
  RepairLegacyPlayerSettings(path);
  RemoveRetiredPlayerSettings(path);
  rex::cvar::LoadConfig(path);
  REXLOG_INFO("Player settings: {}", Utf8(path.filename()));
}

WindowPlacement SavedLauncherPlacement() {
  struct Cvars {
    int GetInt(const char* key, int) const { return std::atoi(rex::cvar::GetFlagByName(key).c_str()); }
    bool GetBool(const char* key, bool) const { return rex::cvar::GetFlagByName(key) == "true"; }
  };
  return ReadPlacement(Cvars{});
}

bool SavedLauncherFullscreen() {
  return REXCVAR_GET(tmnt_launcher_fullscreen);
}

void GameSettings::Capture(const std::filesystem::path& player_file, const std::filesystem::path& shipped_config) {
  player_file_ = player_file;
  shipped_config_ = shipped_config;
  saved_.clear();
  unregistered_.clear();
  for (const std::string& key : kGameSettingKeys) {
    saved_[key] = rex::cvar::GetFlagByName(key);
    if (!rex::cvar::GetFlagInfo(key)) {
      unregistered_.insert(key);
    }
  }
  pending_ = saved_;
}

void GameSettings::CaptureLateRegistered() {
  for (auto it = unregistered_.begin(); it != unregistered_.end();) {
    const std::string& key = *it;
    if (!rex::cvar::GetFlagInfo(key)) {
      ++it;
      continue;
    }
    if (pending_[key].empty()) {
      saved_[key] = pending_[key] = rex::cvar::GetFlagByName(key);
    }
    it = unregistered_.erase(it);
  }
}

std::string GameSettings::Get(std::string_view key, std::string_view def) const {
  const auto it = pending_.find(std::string(key));
  return it == pending_.end() || it->second.empty() ? std::string(def) : it->second;
}

void GameSettings::Set(std::string_view key, std::string value) {
  pending_[std::string(key)] = std::move(value);
}

bool GameSettings::Save() {
  for (const PlayerNameSetting& player : kPlayerNames) {
    const auto name = pending_.find(player.key);
    if (name != pending_.end() && !IsValidPlayerName(name->second)) {
      name->second = player.default_name;
    }
  }
  std::vector<std::pair<std::string, std::string>> changes;
  for (const auto& [key, value] : pending_) {
    const auto saved = saved_.find(key);
    if (saved == saved_.end() || saved->second != value) {
      changes.emplace_back(key, value);
    }
  }
  if (changes.empty()) {
    return true;
  }
  for (const auto& [key, value] : changes) {
    if (!rex::cvar::GetFlagInfo(key)) {
      REXLOG_ERROR("Cannot save {}: the setting is not registered", key);
      return false;
    }
  }

  trg::TextFileSettings file = OpenPlayerFile(player_file_);
  for (const auto& [key, value] : changes) {
    file.Set(key, TomlValue(key, value));
  }
  if (!file.Save()) {
    REXLOG_ERROR("Cannot write player settings to {}", Utf8(player_file_.filename()));
    return false;
  }
  saved_ = pending_;
  return true;
}

bool GameSettings::SaveNow(const SettingValues& values) {
  trg::TextFileSettings file = OpenPlayerFile(player_file_);
  for (const auto& [key, value] : values) {
    file.Set(key, TomlValue(key, value));
  }
  if (!file.Save()) {
    REXLOG_ERROR("Cannot write player settings to {}", Utf8(player_file_.filename()));
    return false;
  }
  for (const auto& [key, value] : values) {
    if (const auto saved = saved_.find(key); saved != saved_.end()) {
      saved->second = value;
    }
  }
  return true;
}

void GameSettings::ResetToDefaults() {
  toml::table shipped;
  try {
    shipped = toml::parse_file(Utf8(shipped_config_));
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("Cannot read {}: {}", Utf8(shipped_config_.filename()), e.what());
  }
  for (const std::string& key : kGameSettingKeys) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
    pending_[key] = ConfigValue(shipped, key).value_or(info ? info->default_value : std::string());
  }
}

void GameSettings::Apply() const {
  for (const auto& [key, value] : pending_) {
    if (IsWindowKey(key) || rex::cvar::GetFlagByName(key) == value) {
      continue;
    }
    rex::cvar::SetFlagByName(key, value);
  }
  ApplyAspectRatio();
  ApplyMultisampling();
  ApplyPlayerNames();
  ApplyLanguage();
}

void ApplyAspectRatio() {
  const AspectRatio aspect = EffectiveAspectRatio(rex::cvar::GetFlagByName("tmnt_aspect_ratio"));
  rex::cvar::SetFlagByName("tmnt_aspect_ratio", InfoFor(aspect).key);
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(VideoModeWidth(aspect)));
  rex::cvar::SetFlagByName("video_mode_height", std::to_string(kVideoModeHeight));
  rex::cvar::SetFlagByName("present_letterbox", "true");
}

// Also replaces a native_2x_msaa = false written by launchers that labelled it Off.
void ApplyMultisampling() {
  rex::cvar::SetFlagByName("native_2x_msaa",
                           Native2xMsaaFor(REXCVAR_GET(tmnt_msaa_compatibility)) ? "true" : "false");
}

void ApplyPlayerNames() {
  for (const PlayerNameSetting& player : kPlayerNames) {
    rex::cvar::SetFlagByName(player.sdk_key, SdkPlayerName(player, rex::cvar::GetFlagByName(player.key)));
  }
}

void ApplyLanguage() {
  const LanguageSetting& language = LanguageFor(rex::cvar::GetFlagByName(kLanguageSetting));
  rex::cvar::SetFlagByName("user_language", std::to_string(language.xbox_language));
  localization::SetActiveLanguage(language.key);
  REXLOG_INFO("Language: {} (user_language {})", language.label, language.xbox_language);
}

void ApplyGraphicsAdapter() {
  const std::string setting = REXCVAR_GET(tmnt_graphics_adapter);
#if !defined(_WIN32)
  // Until a card is chosen in the launcher the loader's and Mesa's own device selection stands.
  if (!ParseGraphicsAdapter(setting)) {
    return;
  }
#endif
  const std::vector<GraphicsAdapter> adapters = ListGraphicsAdapters();
  const std::optional<int> index = ResolveGraphicsAdapter(setting, adapters);
  if (!index) {
    REXLOG_WARN("No usable {} graphics card found", GraphicsBackendName());
    return;
  }
  const auto chosen = std::find_if(adapters.begin(), adapters.end(),
                                   [&](const GraphicsAdapter& adapter) { return adapter.index == *index; });
  if (ParseGraphicsAdapter(setting) && ParseGraphicsAdapter(setting) != index) {
    REXLOG_WARN("Graphics card {} is not available, using {} (adapter {})", setting, chosen->name, *index);
  } else {
    REXLOG_INFO("Graphics card: {} (adapter {})", chosen->name, *index);
  }
  rex::cvar::SetFlagByName(kBackendAdapterCvar, std::to_string(*index));
  rex::cvar::SetFlagByName("tmnt_graphics_adapter", std::to_string(*index));
}

void LogStartupDiagnostics(const GameSettings& settings, const MonitorInfo& display, int monitor,
                           const std::filesystem::path& game_dir, const StartupWindowInfo& window) {
  const AspectRatio aspect = EffectiveAspectRatio(settings.Get("tmnt_aspect_ratio", "16:9"));
  const int scale = std::max(1, std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()));
  const std::string selected_card = SelectedGraphicsCardLabel(settings);
  const bool selected_fullscreen = settings.GetBool("fullscreen", false);
  const bool selected_tearing = settings.GetBool(kTearingSetting, true);
  const bool runtime_tearing = CvarText(kTearingSetting, "true") == "true";

  StartupDiagnostics diagnostics;
  diagnostics.launcher_display.mode = selected_fullscreen ? "Borderless Fullscreen" : "Windowed";
  const int selected_width = window.selected_width >= 0 ? window.selected_width : window.configured_width;
  const int selected_height = window.selected_height >= 0 ? window.selected_height : window.configured_height;
  diagnostics.launcher_display.window_size =
      selected_width > 0 && selected_height > 0
          ? std::to_string(selected_width) + "x" + std::to_string(selected_height)
          : "Default";
  diagnostics.launcher_display.monitor =
      monitor > 0 ? "Display " + std::to_string(monitor) : "Default (primary)";
  diagnostics.launcher_display.aspect_ratio = InfoFor(aspect).label;
  diagnostics.launcher_display.vsync = selected_tearing ? "Off" : "On";

  diagnostics.launcher_gameplay.language = LanguageFor(settings.Get("tmnt_language", kLanguageDefault)).label;
  diagnostics.launcher_gameplay.skip_intro = OnOff(settings.GetBool("tmnt_skip_intro", false));
  diagnostics.launcher_gameplay.frame_rate_in_title = OnOff(settings.GetBool("tmnt_show_fps", true));
  diagnostics.launcher_gameplay.render_resolution_in_title =
      OnOff(settings.GetBool("tmnt_show_render_resolution", false));
  diagnostics.launcher_gameplay.frame_counter =
      settings.GetBool("tmnt_show_frame_counter", false) ? "Shown" : "Hidden";
  diagnostics.launcher_gameplay.classic_soundtrack = OnOff(settings.GetBool("tmnt_classic_soundtrack", false));
  diagnostics.launcher_gameplay.infinite_health = OnOff(settings.GetBool(cheats::kInfiniteHealthSetting, false));
  diagnostics.launcher_gameplay.infinite_lives = OnOff(settings.GetBool(cheats::kInfiniteLivesSetting, false));
  diagnostics.launcher_gameplay.turbo_attack = OnOff(settings.GetBool(cheats::kTurboAttackSetting, false));
  diagnostics.launcher_gameplay.turbo_strong_attack =
      OnOff(settings.GetBool(cheats::kTurboStrongAttackSetting, false));
  const int multiplier = cheats::ParseDamageMultiplier(settings.GetInt(cheats::kDamageMultiplierSetting, 0));
  diagnostics.launcher_gameplay.damage_multiplier = multiplier == 0 ? "Off" : std::to_string(multiplier) + "x";
  diagnostics.launcher_gameplay.super_armor = OnOff(settings.GetBool(cheats::kSuperArmorSetting, false));
  diagnostics.launcher_gameplay.spin_attack = OnOff(settings.GetBool(cheats::kSpinAttackSetting, false));
  diagnostics.launcher_gameplay.unlock_all_stages = OnOff(settings.GetBool(kUnlockAllStagesSetting, false));

  diagnostics.launcher_graphics.graphics_card = selected_card;
  diagnostics.launcher_graphics.render_resolution =
      (scale == 1 ? std::string("Original") : std::to_string(scale) + "x") + " (" +
      std::to_string(GuestWidth(aspect) * scale) + "x" + std::to_string(kGuestHeight * scale) + ")";
  diagnostics.launcher_graphics.anti_aliasing =
      ChoiceLabel({{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA Extreme"}},
                  settings.Get("swap_post_effect", "none"), "Off");
  diagnostics.launcher_graphics.multisampling =
      settings.GetBool("tmnt_msaa_compatibility", false) ? "4x MSAA (Compatibility)" : "2x MSAA";
  diagnostics.launcher_graphics.texture_filtering =
      TextureFilteringName(settings.Get("anisotropic_override", "3"));

  diagnostics.system.os = OsDescription();
  diagnostics.system.cpu = CpuDescription();
  diagnostics.system.logical_processors = LogicalProcessorCount();

  diagnostics.gpu.selected = selected_card;
  const GraphicsAdapterDetails adapter =
      EffectiveGraphicsAdapter(std::atoi(CvarText(kBackendAdapterCvar, "-1").c_str()));
  diagnostics.gpu.resolved = adapter.name;
  diagnostics.gpu.adapter = adapter.index;
  diagnostics.gpu.vendor_id = adapter.vendor_id;
  diagnostics.gpu.device_id = adapter.device_id;
  diagnostics.gpu.resolved_known = adapter.known;

  diagnostics.display.monitor = monitor;
  diagnostics.display.width = display.width;
  diagnostics.display.height = display.height;
  diagnostics.display.refresh_hz = display.refresh_hz;
  diagnostics.display.scale_percent = int(std::lround(display.scale * 100.0f));
  diagnostics.display.fullscreen = window.fullscreen;
  diagnostics.display.configured_width = window.configured_width;
  diagnostics.display.configured_height = window.configured_height;
  diagnostics.display.output_width = window.output_width;
  diagnostics.display.output_height = window.output_height;
  diagnostics.display.aspect = InfoFor(aspect).key;

  const GameCheck game = CheckGameFiles(game_dir);
  diagnostics.game.title_id = kTitleId;
  diagnostics.game.executable = "default.xex";
  std::error_code size_ec;
  const std::uintmax_t file_size = std::filesystem::file_size(game_dir / "default.xex", size_ec);
  diagnostics.game.size = size_ec ? 0 : file_size;
  if (game.xex_crc32 != 0) {
    char crc[16];
    std::snprintf(crc, sizeof(crc), "crc32:%08X", game.xex_crc32);
    diagnostics.game.fingerprint = crc;
  }
  diagnostics.game.status = GameStateName(game.state);
  std::string hash_error;
  if (game.state == GameFiles::kReady) {
    diagnostics.game.sha256 = trg::update::Sha256FileHex(game_dir / "default.xex", hash_error);
  }

  diagnostics.gameplay.present = PresentationText(runtime_tearing);

  diagnostics.graphics.render_width = GuestWidth(aspect) * scale;
  diagnostics.graphics.render_height = kGuestHeight * scale;
  diagnostics.graphics.guest_width = GuestWidth(aspect);
  diagnostics.graphics.guest_height = kGuestHeight;
  diagnostics.graphics.msaa = settings.GetBool("tmnt_msaa_compatibility", false) ? "4x compatibility" : "2x native";
  diagnostics.graphics.anti_aliasing = diagnostics.launcher_graphics.anti_aliasing;
  diagnostics.graphics.texture_filtering = diagnostics.launcher_graphics.texture_filtering;
  diagnostics.graphics.render_target_path = CvarText("render_target_path_d3d12", "default");

  diagnostics.input.keyboard_mode = settings.GetBool("mnk_mode", true) ? "On" : "Off";
  diagnostics.input.controller = window.controller;

  for (const std::string& line : FormatStartupDiagnostics(diagnostics)) {
    REXLOG_INFO("{}", line);
  }
  SetCrashRuntimeState(diagnostics.gpu.resolved_known ? diagnostics.gpu.resolved.c_str() : "unknown",
                       (std::to_string(display.width) + "x" + std::to_string(display.height) + " | window=" +
                        std::to_string(window.configured_width) + "x" + std::to_string(window.configured_height) +
                        " | output=" + std::to_string(window.output_width) + "x" +
                        std::to_string(window.output_height))
                           .c_str(),
                       diagnostics.gameplay.present.c_str(),
                       ("render=" + std::to_string(diagnostics.graphics.render_width) +
                        "x" + std::to_string(diagnostics.graphics.render_height) + " | msaa=" +
                        diagnostics.graphics.msaa)
                           .c_str());
}

void GameSettings::MarkWindowCreated() {
  window_created_.clear();
  for (const std::string& key : kWindowKeys) {
    window_created_[key] = rex::cvar::GetFlagByName(key);
  }
}

bool GameSettings::WindowChanged() const {
  for (const std::string& key : kWindowKeys) {
    const auto created = window_created_.find(key);
    const std::string created_value = created == window_created_.end() ? std::string() : created->second;
    if (WindowValue(key, Get(key)) != WindowValue(key, created_value)) {
      return true;
    }
  }
  return false;
}

}  // namespace tmnt
