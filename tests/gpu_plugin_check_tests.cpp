// Startup refuses to continue without a GPU plugin and says why.
// Run: tmnt_gpu_plugin_check_tests

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gpu_plugin_check.h"

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

std::filesystem::path Write(const std::filesystem::path& dir, const char* text) {
  const std::filesystem::path file = dir / "tmnt_turtles_in_time_reshelled.toml";
  std::ofstream(file, std::ios::binary) << text;
  return file;
}

}  // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tmnt_gpu_plugin_check_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  std::printf("plugin selected\n");
  CHECK(tmnt::MissingGpuPluginMessage("xenos", dir / "tmnt_turtles_in_time_reshelled.toml").empty());

  std::printf("config missing\n");
  CHECK(Has(tmnt::MissingGpuPluginMessage("", dir / "tmnt_turtles_in_time_reshelled.toml"), "was not found"));

  std::printf("config without gpu_plugin\n");
  CHECK(Has(tmnt::MissingGpuPluginMessage("", Write(dir, "mnk_mode = true\n")), "does not select"));

  std::printf("config with gpu_plugin that was not applied\n");
  CHECK(Has(tmnt::MissingGpuPluginMessage("", Write(dir, "gpu_plugin = \"xenos\"\nbroken = \n")), "could not be applied"));

  std::filesystem::remove_all(dir);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
