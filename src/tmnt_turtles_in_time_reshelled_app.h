#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/version.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include "achievement_toast.h"
#include "achievements_overlay.h"
#include "achievement_menu.h"
#include "active_log.h"
#include "aspect_ratio.h"
#include "cheat_menu.h"
#include "diagnostics.h"
#include "frame_counter.h"
#include "game_files.h"
#include "invite_ui.h"
#include "gpu_plugin_check.h"
#include "guest_functions.h"
#include "input_summary.h"
#include "launcher.h"
#include "live_fix.h"
#include "localization/active_language.h"
#include "localization/ui_text.h"
#include "local_stats.h"
#include "log_filter.h"
#include "log_path.h"
#include "modal_dialog.h"
#include "online_runtime.h"
#include "ping_display.h"
#include "platform.h"
#include "player_names.h"
#include "language.h"
#include "port_version.h"
#include "session_log.h"
#include "settings.h"
#include "system_info.h"
#include "classic_soundtrack_overlay.h"
#include "classic_soundtrack_runtime.h"
#include "title_bgm_level.h"
#include "trg/game_helpers.h"
#include "user_data.h"
#include "window_host.h"

class TurtlesInTimeReshelledApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    tmnt::SetApplicationId();
    tmnt::InstallGuestFunctionTable(PPCImageConfig.func_mappings);
    return std::unique_ptr<TurtlesInTimeReshelledApp>(
        new TurtlesInTimeReshelledApp(ctx, "tmnt_turtles_in_time_reshelled", PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    shipped_config_ = paths.config_path;
    const std::filesystem::path port_folder = tmnt::PortFolder();
    if (paths.game_data_root.empty()) {
      paths.game_data_root = port_folder / "game";
    }
    ChooseUserData(paths, port_folder);
    if (const auto inherited =
            tmnt::TakeInheritedSessionLog({port_folder / "logs", paths.user_data_root / "logs"})) {
      rex::cvar::SetFlagFromCommandLine("log_file", tmnt::Utf8(*inherited));
      tmnt::InstallCrashReports();
    }
    // ReXGlue writes its logs beside the executable, which is read-only inside an AppImage.
    const bool appimage = port_folder != rex::filesystem::GetExecutableFolder();
    if (appimage && rex::cvar::GetFlagByName("log_file").empty()) {
      const std::filesystem::path logs = tmnt::LogsFolder(port_folder, paths.user_data_root, {}, appimage);
      if (const std::filesystem::path file = tmnt::NextLogFile(logs, "tmnt_turtles_in_time_reshelled"); !file.empty()) {
        rex::cvar::SetFlagByName("log_file", tmnt::Utf8(file));
      }
    }
  }

  bool SetupPresentation() override {
    if (const std::string problem = tmnt::MissingGpuPluginMessage(rex::cvar::GetFlagByName("gpu_plugin"), shipped_config_);
        !problem.empty()) {
      REXLOG_ERROR("{}", problem);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, tmnt::TrMessage(problem));
      return false;
    }
    if (rex::ReXApp::SetupPresentation()) {
      return true;
    }
    REXLOG_WARN("Graphics unavailable");
    tmnt::RunLauncherWithoutGraphics(DescribeGameFiles());
    return false;
  }

  void OnPostInitLogging() override {
    tmnt::InitializeProcessUptime();
    tmnt::InstallCrashReports();
    const tmnt::UserDataMessage message = tmnt::DescribeUserData(user_data_);
    if (message.warning) {
      REXLOG_WARN("{}", message.text);
    } else {
      REXLOG_INFO("{}", message.text);
    }
    tmnt::LoadPlayerSettings(tmnt::PlayerSettingsPath(user_data_root()));
    // Startup failures are reported before ApplyLanguage runs.
    tmnt::localization::SetActiveLanguage(tmnt::LanguageFor(rex::cvar::GetFlagByName(tmnt::kLanguageSetting)).key);
    tmnt::ApplyDetailedLogging();
    tmnt::InstallSessionLogCleanup();
    tmnt::RemoveDiscardedSessionLogs();
    const tmnt::ApplicationIdentity identity{
        .product = "Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled",
        .version = tmnt::PortVersion(),
        .build_config = tmnt::BuildConfigurationName(),
        .architecture = tmnt::ArchitectureName(),
        .sdk = REXGLUE_VERSION_STRING,
        .backend = tmnt::GraphicsBackendName(),
    };
    REXLOG_INFO("{}", tmnt::FormatApplicationVersion(identity));
    REXLOG_INFO("{}", tmnt::FormatBuildIdentity(identity));
    REXLOG_INFO("{}", tmnt::GameFilesLogText(tmnt::CheckGameFiles(game_data_root())));
    tmnt::ApplyGraphicsAdapter();
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    tmnt::LoadLauncherFonts(atlas, window() ? float(window()->GetDpi()) / float(window()->GetMediumDpi()) : 1.0f);
  }

  void OnDpiScaleChanged(float scale) override {
    tmnt::UpdateLauncherFonts(imgui_drawer(), immediate_drawer(), scale);
  }

  void OnPreSetup(rex::RuntimeConfig&) override {
    game_settings_.Capture(tmnt::PlayerSettingsPath(user_data_root()), shipped_config_);
    show_launcher_ = tmnt::ShouldShowLauncher(game_data_root());
    if (show_launcher_) {
      tmnt::PrepareLauncherWindow();
    }
    game_settings_.MarkWindowCreated();
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (window()) {
      window()->SetTitle(kWindowTitle);
    }
    tmnt::ApplyWindowIcon();
    game_settings_.CaptureLateRegistered();
    if (!show_launcher_ && tmnt::ShiftHeldAfterWindowOpened()) {
      show_launcher_ = true;
    }
    if (!show_launcher_) {
      tmnt::ConfigureOnline(defaults.user_data_root);
      tmnt::ApplyMultisampling();
      tmnt::ApplyPlayerNames();
      tmnt::ApplyLanguage();
      tmnt::ApplyAspectRatio();
      return defaults;
    }
    if (!imgui_drawer()) {
      game_settings_.Apply();
      return defaults;
    }
    REXLOG_INFO("Launcher shown");
    tmnt::LauncherCallbacks callbacks;
    callbacks.play = [this, resume, defaults] {
      app_context().CallInUIThreadDeferred([this, resume, defaults] {
        game_settings_.Apply();
        tmnt::ApplyDetailedLogging();
        resume(defaults);
      });
    };
    callbacks.window_changed = [this] { return game_settings_.WindowChanged(); };
    callbacks.restart = [this] {
      if (!tmnt::RelaunchSelf(L"--tmnt_skip_launcher=true")) {
        return false;
      }
      app_context().CallInUIThreadDeferred([] {
        REXLOG_INFO("Restarted in a new window; hard-exiting this process.");
        rex::FlushLogging();
        std::_Exit(0);
      });
      return true;
    };
    callbacks.quit = [this] { app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); }); };
    callbacks.window_handle = [this] { return window() ? window()->GetNativeWindowHandle() : nullptr; };
    callbacks.window_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    callbacks.set_window_fullscreen = [this](bool fullscreen) {
      if (window()) {
        window()->SetFullscreen(fullscreen);
      }
    };
    const std::filesystem::path port_folder = tmnt::PortFolder();
    const std::filesystem::path logs_dir =
        tmnt::LogsFolder(port_folder, defaults.user_data_root, tmnt::ActiveLogFile(),
                        port_folder != rex::filesystem::GetExecutableFolder());
    tmnt::ShowLauncher(imgui_drawer(),
                      {defaults.game_data_root, defaults.user_data_root,
                       tmnt::PlayerSettingsPath(defaults.user_data_root), logs_dir, user_data_.outcome,
                       immediate_drawer()},
                      game_settings_, std::move(callbacks));
    return std::nullopt;
  }

  void LaunchModule() override {
    const int monitor = game_settings_.GetInt("monitor", std::atoi(rex::cvar::GetFlagByName("monitor").c_str()));
    tmnt::StartupWindowInfo startup_window;
    startup_window.fullscreen = window() != nullptr && window()->IsFullscreen();
    startup_window.selected_width = game_settings_.GetInt(
        "window_width", std::atoi(rex::cvar::GetFlagByName("window_width").c_str()));
    startup_window.selected_height = game_settings_.GetInt(
        "window_height", std::atoi(rex::cvar::GetFlagByName("window_height").c_str()));
    startup_window.configured_width = window() != nullptr ? int(window()->GetDesiredLogicalWidth())
                                                          : startup_window.selected_width;
    startup_window.configured_height = window() != nullptr ? int(window()->GetDesiredLogicalHeight())
                                                           : startup_window.selected_height;
    const tmnt::WindowSize drawable = CurrentDrawableSize(window());
    startup_window.output_width = drawable.width;
    startup_window.output_height = drawable.height;
    startup_window.controller = tmnt::ControllerSummary();
    tmnt::LogStartupDiagnostics(game_settings_, tmnt::MonitorForSetting(monitor), monitor, game_data_root(),
                               startup_window);
    tmnt::PublishTitleMusicLevel(user_data_root());
    tmnt::StartClassicSoundtrack(user_data_root());
    ReXApp::LaunchModule();
  }

  // ReXGlue's debug (F3) and settings (F4) overlays are developer tools; F3 is the Ping display here.
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    for (const char* bind : tmnt::kRemovedSdkBinds) rex::ui::UnregisterBind(bind);
  }

  void OnPostSetup() override {
    if (runtime() && runtime()->kernel_state() && runtime()->input_system()) {
      tmnt::InstallAchievementMenu({
          .open = [this] { app_context().CallInUIThreadDeferred([this] { OpenAchievements(); }); },
          .close = [this] { app_context().CallInUIThreadDeferred([this] { RequestCloseOverlay(); }); },
      });
      static_cast<rex::input::InputSystem*>(runtime()->input_system())
          ->SetActiveCallback([this] {
            if (tmnt::AchievementMenuHoldsInput()) return true;
            return !tmnt::ModalDialog::AnyOpen() && (!imgui_drawer() || !imgui_drawer()->GetIO().WantCaptureMouse);
          });
      if (auto* kernel = runtime()->kernel_state(); kernel->user_profile()) {
        const auto* profile = kernel->user_profile();
        tmnt::live::Install({
            .stats_file = tmnt::StatsFilePath(std::filesystem::absolute(user_data_root()), kernel->title_id()),
            .user = {profile->xuid(), rex::cvar::GetFlagByName(tmnt::kPlayerNames[0].sdk_key)},
        });
      }
    }
#if defined(_WIN32)
    REXLOG_INFO("Scheduling: {}", trg::TuneProcessScheduling());
#endif
    AttachWindowHost(window(), app_context(), [this] {
      rex::ui::ImGuiDrawer* drawer = imgui_drawer();
      return drawer && drawer->GetIO().WantCaptureMouse;
    });
    if (window()) {
      tmnt::CheckWindowDisplay(window()->GetNativeWindowHandle(), REXCVAR_GET(monitor));
    }
    if (window() && imgui_drawer()) {
      tmnt::CreateFrameCounter(imgui_drawer());
      tmnt::CreateClassicSoundtrackOverlay(imgui_drawer());
      tmnt::CreatePingDisplay(imgui_drawer());
      tmnt::CreateInviteUi(imgui_drawer(), [this](std::function<void()> work) {
        app_context().CallInUIThreadDeferred(std::move(work));
      });
      CreateCursorSync(imgui_drawer());
      tmnt::ModalDialog::AttachEscapeKey(window());
      cheat_menu_.Attach(window(), imgui_drawer());
    }
  }

  void OnShutdown() override {
    // ReXApp destroys the window before the runtime; the input drivers must detach while it still exists.
    if (auto* input = runtime() ? runtime()->input_system() : nullptr) {
      input->Shutdown();
    }
    cheat_menu_.Detach();
    tmnt::ModalDialog::DetachEscapeKey();
    tmnt::StopOnline();
    CloseAchievements();
    DetachWindowHost();
    REXLOG_INFO("{}", tmnt::FormatCleanExit(tmnt::ProcessUptimeText()));
  }

  // Window close is a hard exit: the base TerminateTitle would call StopTitleThreads and wait on guest
  // threads that are still running guest code. The OS reclaims the process.
  void OnClosing(rex::ui::UIEvent&) override {
    REXLOG_INFO("Window closing, hard-exiting process.");
    tmnt::DiscardSessionLogBeforeHardExit();
    tmnt::PrepareHardExit();
    std::_Exit(0);
  }

  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    if (!tmnt::localization::PortugueseBrazilSelected() || !imgui_drawer() || !immediate_drawer() || !runtime()) {
      return ReXApp::CreateAchievementNotificationDialog();
    }
    return std::make_unique<tmnt::AchievementToast>(imgui_drawer(), immediate_drawer(), runtime());
  }

  // F7 toggles the achievements overlay. The overlay is owned here, not by ReXApp.
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override {
    if (achievements_) {
      achievements_->RequestClose();
    } else {
      OpenAchievements();
    }
    return nullptr;
  }

 private:
  tmnt::GraphicsFallbackInfo DescribeGameFiles() const {
    switch (tmnt::CheckGameFiles(game_data_root()).state) {
      case tmnt::GameFiles::kReady:
        return {tmnt::Utf8(game_data_root()), "Ready", true};
      case tmnt::GameFiles::kUnsupported:
        return {tmnt::Utf8(game_data_root()), "Unsupported version", false};
      default:
        return {tmnt::Utf8(game_data_root()), "Setup required", false};
    }
  }

  // The cache folder ReXGlue derived from the system root follows the user data root unless it was set explicitly.
  void ChooseUserData(rex::PathConfig& paths, const std::filesystem::path& port_folder) {
    user_data_ = tmnt::SelectUserData({port_folder, paths.user_data_root, !REXCVAR_GET(user_data_root).empty()});
    if (user_data_.root == paths.user_data_root) {
      return;
    }
    paths.user_data_root = user_data_.root;
    if (REXCVAR_GET(cache_root).empty()) {
      paths.cache_root = user_data_.root / "cache";
    }
  }

  void OpenAchievements() {
    if (achievements_ || tmnt::ModalDialog::AnyOpen() || !runtime() || !runtime()->kernel_state() || !imgui_drawer() ||
        !immediate_drawer()) {
      return;
    }
    achievements_ = new tmnt::AchievementsOverlay(imgui_drawer(), immediate_drawer(), runtime(), &achievements(),
                                                  [this] {
                                                    achievements_ = nullptr;
                                                    tmnt::SetAchievementMenuOpen(false);
                                                  });
    tmnt::SetAchievementMenuOpen(true);
  }

  void CloseAchievements() {
    if (!achievements_) {
      tmnt::SetAchievementMenuOpen(false);
      return;
    }
    if (achievements_->Closing()) {
      return;
    }
    delete achievements_;
  }

  void RequestCloseOverlay() {
    if (achievements_) {
      achievements_->RequestClose();
    }
  }

  tmnt::GameSettings game_settings_;
  tmnt::AchievementsOverlay* achievements_ = nullptr;
  tmnt::CheatMenu cheat_menu_;
  std::filesystem::path shipped_config_;
  tmnt::UserDataSelection user_data_;
  bool show_launcher_ = false;
};
