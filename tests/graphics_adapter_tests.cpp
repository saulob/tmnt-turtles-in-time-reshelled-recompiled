// Graphics card: the setting values, the adapter list ReXGlue's d3d12_adapter or vulkan_device index refers to,
// persistence and where the setting is registered.
// Run: tmnt_graphics_adapter_tests

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include "graphics_adapters.h"
#include "trg/settings.h"

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

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

void TestValues() {
  std::printf("values\n");
  using tmnt::ParseGraphicsAdapter;
  CHECK(!ParseGraphicsAdapter("auto"));
  CHECK(!ParseGraphicsAdapter(""));
  CHECK(!ParseGraphicsAdapter("-1"));
  CHECK(!ParseGraphicsAdapter("1x"));
  CHECK(!ParseGraphicsAdapter("Intel"));
  CHECK(ParseGraphicsAdapter("0") == 0);
  CHECK(ParseGraphicsAdapter("1") == 1);
  CHECK(ParseGraphicsAdapter("12") == 12);

  const std::vector<tmnt::GraphicsAdapter> adapters = {{0, "NVIDIA GeForce RTX 4070"}, {1, "Intel UHD Graphics 730"}};
  CHECK(tmnt::ResolveGraphicsAdapter("auto", adapters) == 0);
  CHECK(tmnt::ResolveGraphicsAdapter("", adapters) == 0);
  CHECK(tmnt::ResolveGraphicsAdapter("0", adapters) == 0);
  CHECK(tmnt::ResolveGraphicsAdapter("1", adapters) == 1);
  CHECK(tmnt::ResolveGraphicsAdapter("2", adapters) == 0);
  CHECK(!tmnt::ResolveGraphicsAdapter("1", {}));
  CHECK(!tmnt::ResolveGraphicsAdapter("auto", {}));
  // The index is the DXGI one, not the position in the list.
  const std::vector<tmnt::GraphicsAdapter> gap = {{1, "A"}, {3, "B"}};
  CHECK(tmnt::ResolveGraphicsAdapter("1", gap) == 1);
  CHECK(tmnt::ResolveGraphicsAdapter("3", gap) == 3);
  CHECK(tmnt::ResolveGraphicsAdapter("0", gap) == 1);
  CHECK(tmnt::ResolveGraphicsAdapter("auto", gap) == 1);

  auto options = tmnt::GraphicsAdapterOptions(adapters);
  CHECK(options.size() == 2);
  CHECK(options[0].first == "0" && options[0].second == "NVIDIA GeForce RTX 4070");
  CHECK(options[1].first == "1" && options[1].second == "Intel UHD Graphics 730");
  for (const auto& [value, label] : options) {
    CHECK(value != "auto" && label.find("Automatic") == std::string::npos);
  }
  CHECK(tmnt::GraphicsAdapterOptions({}).empty());
  options = tmnt::GraphicsAdapterOptions({{0, "Same"}, {1, "Same"}});
  CHECK(options[0].second == "Same (0)" && options[1].second == "Same (1)");
}

void TestPersistence() {
  std::printf("save and reload\n");
  const std::filesystem::path file = std::filesystem::temp_directory_path() / "tmnt_graphics_adapter_test.toml";
  std::filesystem::remove(file);
  {
    std::ofstream(file) << "# Turtles in Time Re-Shelled player settings\nresolution_scale = 2\ntmnt_show_fps = false\n";
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(!settings.Has("tmnt_graphics_adapter"));
    CHECK(tmnt::ResolveGraphicsAdapter(settings.Get("tmnt_graphics_adapter", ""), {{0, "A"}, {1, "B"}}) == 0);
    settings.Set("tmnt_graphics_adapter", "\"1\"");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.Get("tmnt_graphics_adapter") == "\"1\"");
    CHECK(!settings.GetBool("tmnt_show_fps", true));
    settings.Set("tmnt_graphics_adapter", "\"auto\"");
    CHECK(settings.Save());
  }
  {
    trg::TextFileSettings settings(file.string());
    CHECK(settings.Get("tmnt_graphics_adapter") == "\"auto\"");
    CHECK(settings.GetInt("resolution_scale", 1) == 2);
  }
  std::filesystem::remove(file);
}

#if defined(_WIN32)
// ReXGlue opens d3d12_adapter = N as the Nth adapter of EnumAdapters1 on a CreateDXGIFactory2 factory.
void TestEnumerationMatchesRuntime() {
  std::printf("enumeration against the runtime's adapter order\n");
  const std::vector<tmnt::GraphicsAdapter> adapters = tmnt::ListGraphicsAdapters();
  for (const tmnt::GraphicsAdapter& adapter : adapters) {
    std::printf("  %d  %s\n", adapter.index, adapter.name.c_str());
  }
  CHECK(!adapters.empty());

  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
  if (!factory) {
    return;
  }
  for (size_t i = 0; i < adapters.size(); ++i) {
    CHECK(i == 0 || adapters[i].index > adapters[i - 1].index);
    CHECK(!adapters[i].name.empty());
    CHECK(adapters[i].name != "Microsoft Basic Render Driver");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapters1(UINT(adapters[i].index), &adapter) == S_OK);
    if (!adapter) {
      continue;
    }
    DXGI_ADAPTER_DESC1 desc;
    CHECK(SUCCEEDED(adapter->GetDesc1(&desc)));
    CHECK(!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    const int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    std::string runtime_name(size > 1 ? size_t(size - 1) : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, runtime_name.data(), size, nullptr, nullptr);
    CHECK(runtime_name == adapters[i].name);
  }
  // Every listed adapter was present, and no hardware adapter that can create a device was left out.
  UINT hardware = 0;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index, adapter.Reset()) {
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
      ++hardware;
    }
  }
  CHECK(hardware == adapters.size());
}
#endif

#if !defined(_WIN32)
using tmnt::GraphicsAdapter;

std::string Values(const std::vector<GraphicsAdapter>& adapters) {
  std::string out;
  for (const GraphicsAdapter& adapter : adapters) {
    out += std::to_string(adapter.index) + "=" + adapter.name + ";";
  }
  return out;
}

// The production enumeration against a stand-in driver. ReXGlue opens vulkan_device = N as the Nth
// vkEnumeratePhysicalDevices entry, so the index is the driver's, not the sorted position.
void TestVulkanEnumeration() {
  std::printf("Vulkan device list through a stand-in driver\n");
  const char* driver = TMNT_FAKE_VULKAN_LIBRARY;
  setenv("TMNT_FAKE_VULKAN", "integrated:Intel UHD;cpu:llvmpipe;discrete:NVIDIA RTX;virtual:Hyper-V GPU", 1);
  unsetenv("TMNT_FAKE_VULKAN_FAIL");
  const std::vector<GraphicsAdapter> adapters = tmnt::ListVulkanAdapters(driver);
  CHECK(Values(adapters) == "2=NVIDIA RTX;0=Intel UHD;3=Hyper-V GPU;");
  CHECK(tmnt::EffectiveVulkanAdapter(driver, -1).index == 2);
  CHECK(tmnt::EffectiveVulkanAdapter(driver, -1).name == "NVIDIA RTX");
  CHECK(tmnt::EffectiveVulkanAdapter(driver, 0).name == "Intel UHD");
  CHECK(tmnt::EffectiveVulkanAdapter(driver, 3).known);
  CHECK(!tmnt::EffectiveVulkanAdapter(driver, 1).known);
  CHECK(!tmnt::EffectiveVulkanAdapter(driver, 9).known);

  std::printf("launcher choice to vulkan_device\n");
  const auto options = tmnt::GraphicsAdapterOptions(adapters);
  CHECK(options.size() == 3 && options[0].first == "2" && options[0].second == "NVIDIA RTX" && options[1].first == "0");
  CHECK(tmnt::ResolveGraphicsAdapter("auto", adapters) == 2);
  CHECK(tmnt::ResolveGraphicsAdapter("0", adapters) == 0);
  CHECK(tmnt::ResolveGraphicsAdapter("1", adapters) == 2);

  std::printf("no Vulkan device\n");
  setenv("TMNT_FAKE_VULKAN", "cpu:llvmpipe", 1);
  CHECK(tmnt::ListVulkanAdapters(driver).empty());
  CHECK(!tmnt::EffectiveVulkanAdapter(driver, -1).known);
  unsetenv("TMNT_FAKE_VULKAN");
  CHECK(tmnt::ListVulkanAdapters(driver).empty());
  setenv("TMNT_FAKE_VULKAN", "discrete:NVIDIA RTX", 1);
  setenv("TMNT_FAKE_VULKAN_FAIL", "create", 1);
  CHECK(tmnt::ListVulkanAdapters(driver).empty());
  unsetenv("TMNT_FAKE_VULKAN_FAIL");
  CHECK(tmnt::ListVulkanAdapters("libtmnt-no-such-vulkan.so.1").empty());
  CHECK(tmnt::ListVulkanAdapters("libc.so.6").empty());
  CHECK(!tmnt::EffectiveVulkanAdapter("libtmnt-no-such-vulkan.so.1", -1).known);
}
#endif

void TestRegistration() {
  std::printf("setting, launcher row and startup\n");
  const std::string src = TMNT_SRC_DIR;
  const std::string settings = ReadFile(src + "/settings.cpp");
  CHECK(Has(settings, "REXCVAR_DEFINE_STRING(tmnt_graphics_adapter, \"auto\","));
  CHECK(Has(settings, "      \"tmnt_graphics_adapter\",\n      \"tmnt_aspect_ratio\",") ||
        Has(settings, "      \"tmnt_graphics_adapter\",\r\n      \"tmnt_aspect_ratio\","));
  CHECK(Has(settings, "      kTearingSetting,\n      \"tmnt_graphics_adapter\",\n") ||
        Has(settings, "      kTearingSetting,\r\n      \"tmnt_graphics_adapter\",\r\n"));
  CHECK(Has(settings, "rex::cvar::SetFlagByName(kBackendAdapterCvar, std::to_string(*index));"));

  const std::string launcher = ReadFile(src + "/launcher.cpp");
  const size_t card = launcher.find("ui.Combo(\"Graphics card\"");
  const size_t resolution = launcher.find("ui.Combo(\"Render resolution\"");
  CHECK(card != std::string::npos && resolution != std::string::npos && card < resolution);
  CHECK(Has(launcher, "\"tmnt_graphics_adapter\", value.c_str(), adapters"));
  CHECK(!Has(launcher, "kGraphicsAdapterAuto") && !Has(launcher, "Automatic (Recommended)"));

  const std::string app = ReadFile(src + "/tmnt_turtles_in_time_reshelled_app.h");
  const size_t load = app.find("tmnt::LoadPlayerSettings(");
  const size_t apply = app.find("tmnt::ApplyGraphicsAdapter();");
  CHECK(load != std::string::npos && apply != std::string::npos && load < apply);
}

}  // namespace

int main() {
  TestValues();
  TestPersistence();
#if defined(_WIN32)
  TestEnumerationMatchesRuntime();
#else
  TestVulkanEnumeration();
#endif
  TestRegistration();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
