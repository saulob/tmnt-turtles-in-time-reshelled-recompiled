// Startup diagnostic formatting: port identity, selected versus resolved GPU, launcher selections versus
// effective window state, game identity without paths, timing wording and privacy. Run: tmnt_diagnostics_tests

#include <cstdio>
#include <string>
#include <vector>

#include "diagnostics.h"

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

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    text += '\n';
  }
  return text;
}

tmnt::StartupDiagnostics Sample() {
  tmnt::StartupDiagnostics diagnostics;
  diagnostics.launcher_display.mode = "Borderless Fullscreen";
  diagnostics.launcher_display.window_size = "1280x720";
  diagnostics.launcher_display.monitor = "Display 1";
  diagnostics.launcher_display.aspect_ratio = "16:9 Original";
  diagnostics.launcher_display.vsync = "Off";
  diagnostics.launcher_gameplay.language = "English";
  diagnostics.launcher_gameplay.frame_rate_in_title = "On";
  diagnostics.launcher_gameplay.render_resolution_in_title = "Off";
  diagnostics.launcher_gameplay.frame_counter = "Hidden";
  diagnostics.launcher_graphics.graphics_card = "Automatic";
  diagnostics.launcher_graphics.render_resolution = "2x (2560x1440)";
  diagnostics.launcher_graphics.internal_render_resolution = "100% Native";
  diagnostics.launcher_graphics.anti_aliasing = "Off";
  diagnostics.launcher_graphics.multisampling = "2x MSAA";
  diagnostics.launcher_graphics.texture_filtering = "4x";
  diagnostics.system.os = "Windows 11 64-bit (build 22631)";
  diagnostics.system.cpu = "Test CPU";
  diagnostics.system.logical_processors = 16;
  diagnostics.gpu.selected = "Automatic";
  diagnostics.gpu.resolved = "NVIDIA GeForce RTX 4070";
  diagnostics.gpu.adapter = 0;
  diagnostics.gpu.vendor_id = 0x10DE;
  diagnostics.gpu.device_id = 0x2786;
  diagnostics.gpu.resolved_known = true;
  diagnostics.display.monitor = 1;
  diagnostics.display.width = 2560;
  diagnostics.display.height = 1440;
  diagnostics.display.refresh_hz = 144;
  diagnostics.display.scale_percent = 225;
  diagnostics.display.fullscreen = true;
  diagnostics.display.configured_width = 1280;
  diagnostics.display.configured_height = 720;
  diagnostics.display.output_width = 2560;
  diagnostics.display.output_height = 1440;
  diagnostics.display.aspect = "16:9";
  diagnostics.game.title_id = 0x584109D2;
  diagnostics.game.executable = "default.xex";
  diagnostics.game.size = 4808704;
  diagnostics.game.sha256 = "ABCDEF";
  diagnostics.game.fingerprint = "crc32:FC04059D";
  diagnostics.game.status = "verified";
  diagnostics.gameplay.present = "guest 60Hz | host VBlank (VSync On)";
  diagnostics.graphics.render_percent = 100;
  diagnostics.graphics.render_width = 1280;
  diagnostics.graphics.render_height = 720;
  diagnostics.graphics.guest_width = 1280;
  diagnostics.graphics.guest_height = 720;
  diagnostics.graphics.host_quarters = 4;
  diagnostics.graphics.resolve_scale = 1;
  diagnostics.graphics.msaa = "2x native";
  diagnostics.graphics.anti_aliasing = "Off";
  diagnostics.graphics.texture_filtering = "4x";
  diagnostics.graphics.render_target_path = "default";
  diagnostics.input.keyboard_mode = "On";
  diagnostics.input.controller = "Xbox One S Controller";
  return diagnostics;
}

void TestIdentity() {
  std::printf("port and build identity\n");
  const tmnt::ApplicationIdentity identity{
      .product = "Turtles in Time Re-Shelled",
      .version = "0.6.1",
      .build_config = "RelWithDebInfo",
      .architecture = "x86-64",
      .sdk = "0.10.0",
      .backend = "Direct3D 12",
  };
  CHECK(tmnt::FormatApplicationVersion(identity) == "Turtles in Time Re-Shelled port 0.6.1");
  const std::string build = tmnt::FormatBuildIdentity(identity);
  CHECK(Has(build, "Build: RelWithDebInfo x86-64"));
  CHECK(Has(build, "ReXGlue SDK 0.10.0"));
  CHECK(Has(build, "Direct3D 12"));
}

void TestStartupBlock() {
  std::printf("startup snapshot carries launcher and runtime fields\n");
  const std::string text = Join(tmnt::FormatStartupDiagnostics(Sample()));
  CHECK(Has(text, "Runtime configuration:"));
  CHECK(Has(text, "Launcher Display: Display mode=Borderless Fullscreen | Window size=1280x720 | "
                  "Monitor=Display 1 | Aspect Ratio=16:9 Original | VSync=Off"));
  CHECK(Has(text, "Launcher Gameplay: Language=English | Frame rate in title=On | "
                  "Render resolution in title=Off | Frame counter=Hidden"));
  CHECK(Has(text, "Launcher Graphics: Graphics card=Automatic | Render resolution=2x (2560x1440) | "
                  "Internal render resolution=100% Native | Anti-aliasing=Off | Multisampling=2x MSAA | "
                  "Texture filtering=4x"));
  CHECK(Has(text, "Runtime OS: Windows 11 64-bit (build 22631)"));
  CHECK(Has(text, "Runtime CPU: Test CPU | logical processors=16"));
  CHECK(Has(text, "Runtime GPU: selected=Automatic | resolved=NVIDIA GeForce RTX 4070 | adapter=0 | "
                  "vendor/device=10DE/2786"));
  CHECK(Has(text, "Runtime display: 2560x1440@144Hz | scale=225% | monitor=1 | fullscreen | window=1280x720 | "
                  "output=2560x1440 | aspect=16:9"));
  CHECK(Has(text, "Runtime game: title_id=584109D2 | default.xex | size=4808704 | "
                  "fingerprint=crc32:FC04059D | status=verified"));
  CHECK(Has(text, "Runtime game sha256: ABCDEF"));
  CHECK(Has(text, "Runtime timing: present=guest 60Hz | host VBlank (VSync On)"));
  CHECK(Has(text, "Runtime renderer: render=100% 1280x720 | guest=1280x720 | host_targets=4/4 | resolve=1x | "
                  "msaa=2x native | anti-aliasing=Off | texture filtering=4x | target=default"));
  CHECK(Has(text, "Runtime input: keyboard_mode=On | controller=Xbox One S Controller"));
}

void TestSelectedWindowResolution() {
  std::printf("selected window size differs from the effective window\n");
  tmnt::StartupDiagnostics diagnostics = Sample();
  diagnostics.launcher_display.mode = "Windowed";
  diagnostics.launcher_display.window_size = "1260x540";
  diagnostics.launcher_display.aspect_ratio = "21:9 Ultrawide";
  diagnostics.display.fullscreen = false;
  diagnostics.display.configured_width = 1152;
  diagnostics.display.configured_height = 493;
  diagnostics.display.output_width = 1728;
  diagnostics.display.output_height = 740;
  diagnostics.display.aspect = "21:9";
  const std::string text = Join(tmnt::FormatStartupDiagnostics(diagnostics));
  CHECK(Has(text, "Launcher Display: Display mode=Windowed | Window size=1260x540 | "
                  "Monitor=Display 1 | Aspect Ratio=21:9 Ultrawide | VSync=Off"));
  CHECK(Has(text, "Runtime display: 2560x1440@144Hz | scale=225% | monitor=1 | windowed | "
                  "window=1152x493 | output=1728x740 | aspect=21:9"));
}

void TestSelectedAndResolvedDiffer() {
  std::printf("selected and resolved GPU stay distinguishable\n");
  tmnt::StartupDiagnostics diagnostics = Sample();
  diagnostics.launcher_graphics.graphics_card = "Intel UHD 730";
  diagnostics.gpu.selected = "Intel UHD 730";
  diagnostics.gpu.resolved = "NVIDIA GeForce RTX 4070";
  const std::string text = Join(tmnt::FormatStartupDiagnostics(diagnostics));
  CHECK(Has(text, "Launcher Graphics: Graphics card=Intel UHD 730"));
  CHECK(Has(text, "Runtime GPU: selected=Intel UHD 730"));
  CHECK(Has(text, "resolved=NVIDIA GeForce RTX 4070"));

  diagnostics.gpu.resolved_known = false;
  CHECK(Has(Join(tmnt::FormatStartupDiagnostics(diagnostics)), "resolved=unknown"));

  tmnt::StartupDiagnostics automatic = Sample();
  automatic.launcher_graphics.graphics_card = "Automatic";
  automatic.gpu.selected = "Automatic";
  automatic.gpu.resolved = "NVIDIA GeForce RTX 4070";
  const std::string automatic_text = Join(tmnt::FormatStartupDiagnostics(automatic));
  CHECK(Has(automatic_text, "Launcher Graphics: Graphics card=Automatic"));
  CHECK(Has(automatic_text, "Runtime GPU: selected=Automatic | resolved=NVIDIA GeForce RTX 4070"));
}

void TestPresentationWording() {
  std::printf("guest presentation and host vsync are named separately\n");
  CHECK(tmnt::PresentationText(false) == "guest 60Hz | host VBlank (VSync On)");
  CHECK(tmnt::PresentationText(true) == "guest 60Hz | host Immediate (tearing allowed)");
}

void TestCleanExit() {
  std::printf("the clean-exit line carries the uptime\n");
  CHECK(tmnt::FormatCleanExit("01:02:03") == "Execution complete: clean exit, uptime 01:02:03");
}

void TestPrivacy() {
  std::printf("absolute paths are reduced to file names\n");
  tmnt::StartupDiagnostics diagnostics = Sample();
  diagnostics.game.executable = "C:\\Users\\Example\\Documents\\Games\\default.xex";
  const std::string text = Join(tmnt::FormatStartupDiagnostics(diagnostics));
  CHECK(Has(text, "default.xex"));
  CHECK(!Has(text, "C:\\Users"));
  CHECK(!Has(text, "Example"));
  CHECK(!Has(text, "Documents"));

  CHECK(tmnt::FileNameOnly("C:\\Users\\Example\\Documents\\Games\\default.xex") == "default.xex");
  CHECK(tmnt::FileNameOnly("/home/example/games/default.xex") == "default.xex");
  CHECK(tmnt::FileNameOnly("default.xex") == "default.xex");
}

}  // namespace

int main() {
  TestIdentity();
  TestStartupBlock();
  TestSelectedWindowResolution();
  TestSelectedAndResolvedDiffer();
  TestPresentationWording();
  TestCleanExit();
  TestPrivacy();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
