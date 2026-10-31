// Recompiled module teardown: the bookkeeping of KernelState::LoadUserModule, UnloadUserModule and ~KernelState
// (patch 0083), modelled without a runtime, and checks that patches 0083 and 0085 keep the module's load key and the executable's heap region.
// The model has two unload lookups: the stock one by the module's absolute path and the patched one by load key.
// Run: tmnt_module_teardown_tests

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

std::string NormalizeGuestPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  const auto colon = path.find(':');
  if (colon != std::string::npos && colon + 1 < path.size() && path[colon + 1] == '/') {
    path.erase(0, colon + 2);
  }
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return path;
}

constexpr char kManifestKey[] = "tmnt_r.xex";
constexpr char kRequestPath[] = "game:\\TMNT_r.xex";
constexpr char kAbsolutePath[] = "\\Device\\Harddisk0\\Partition1\\TMNT_r.xex";

enum class Lookup { kByAbsolutePath, kByLoadKey };

struct Library {
  explicit Library(int* live) : live_(live) { ++*live_; }
  ~Library() { --*live_; }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  int* live_;
};

struct Module {
  std::string absolute_path;
  std::string load_key;
};

class Kernel {
 public:
  explicit Kernel(Lookup lookup) : lookup_(lookup) { registered_.push_back(kManifestKey); }

  std::shared_ptr<Module> Load(const std::string& request_path) {
    const std::string key = NormalizeGuestPath(request_path);
    if (std::find(registered_.begin(), registered_.end(), key) == registered_.end()) return nullptr;
    if (libraries_.count(key)) return nullptr;
    libraries_.emplace(key, std::make_unique<Library>(&live_libraries_));
    auto module = std::make_shared<Module>(Module{kAbsolutePath, key});
    modules_.push_back(module);
    return module;
  }

  bool Unload(const std::shared_ptr<Module>& module) {
    auto found = std::find(modules_.begin(), modules_.end(), module);
    const std::string key = lookup_ == Lookup::kByLoadKey ? module->load_key : NormalizeGuestPath(module->absolute_path);
    if (lookup_ == Lookup::kByLoadKey) module->load_key.clear();
    if (auto library = libraries_.find(key); library != libraries_.end()) {
      deferred_.push_back(std::move(library->second));
      libraries_.erase(library);
    }
    if (found == modules_.end()) return false;
    modules_.erase(found);
    return true;
  }

  // ~KernelState up to the check; returns what the check reports.
  size_t Sweep() {
    while (!modules_.empty()) {
      Unload(modules_.back());
    }
    const size_t left = libraries_.size();
    libraries_.clear();
    return left;
  }

  void DrainDeferred() { deferred_.clear(); }

  size_t loaded_libraries() const { return libraries_.size(); }
  size_t modules() const { return modules_.size(); }
  int live_libraries() const { return live_libraries_; }

 private:
  Lookup lookup_;
  std::vector<std::string> registered_;
  std::map<std::string, std::unique_ptr<Library>> libraries_;
  std::vector<std::unique_ptr<Library>> deferred_;
  std::vector<std::shared_ptr<Module>> modules_;
  int live_libraries_ = 0;
};

void AbsolutePathNeverMatchesManifestKey() {
  CHECK(NormalizeGuestPath(kRequestPath) == kManifestKey);
  CHECK(NormalizeGuestPath(kAbsolutePath) != kManifestKey);
}

void StockLookupLeavesTheLibraryLoaded() {
  Kernel kernel(Lookup::kByAbsolutePath);
  CHECK(kernel.Load(kRequestPath));
  CHECK(kernel.Sweep() == 1);
  CHECK(kernel.modules() == 0);
}

void LoadKeyLookupLeavesNoLibraryAfterSweep() {
  Kernel kernel(Lookup::kByLoadKey);
  CHECK(kernel.Load(kRequestPath));
  CHECK(kernel.Sweep() == 0);
  CHECK(kernel.loaded_libraries() == 0);
}

void UnloadedByTheTitleBeforeTheSweep() {
  Kernel kernel(Lookup::kByLoadKey);
  auto module = kernel.Load(kRequestPath);
  CHECK(kernel.Unload(module));
  CHECK(kernel.loaded_libraries() == 0);
  CHECK(kernel.Sweep() == 0);
}

void SecondUnloadIsRejected() {
  Kernel kernel(Lookup::kByLoadKey);
  auto module = kernel.Load(kRequestPath);
  CHECK(kernel.Unload(module));
  CHECK(!kernel.Unload(module));
  CHECK(kernel.live_libraries() == 1);
  kernel.DrainDeferred();
  CHECK(kernel.live_libraries() == 0);
}

void LibraryIsFreedExactlyOnceAndLast() {
  Kernel kernel(Lookup::kByLoadKey);
  auto module = kernel.Load(kRequestPath);
  CHECK(kernel.live_libraries() == 1);
  kernel.Sweep();
  CHECK(kernel.live_libraries() == 1);
  kernel.DrainDeferred();
  CHECK(kernel.live_libraries() == 0);
  kernel.DrainDeferred();
  CHECK(kernel.live_libraries() == 0);
}

void DuplicateLoadIsRefused() {
  Kernel kernel(Lookup::kByLoadKey);
  CHECK(kernel.Load(kRequestPath));
  CHECK(!kernel.Load(kRequestPath));
  CHECK(kernel.loaded_libraries() == 1);
}

void ReloadAfterUnloadWorks() {
  Kernel kernel(Lookup::kByLoadKey);
  auto first = kernel.Load(kRequestPath);
  CHECK(kernel.Unload(first));
  auto second = kernel.Load(kRequestPath);
  CHECK(second);
  CHECK(kernel.Sweep() == 0);
}

// The order Runtime::Shutdown (patch 0033) and ~KernelState give: title threads, then the module sweep, then the
// count check, then the library release.
void ShutdownOrderIsRecorded() {
  std::vector<std::string> steps;
  Kernel kernel(Lookup::kByLoadKey);
  kernel.Load(kRequestPath);
  steps.push_back("stop title threads");
  steps.push_back("module sweep");
  const size_t left = kernel.Sweep();
  steps.push_back("module count " + std::to_string(left));
  steps.push_back("free libraries");
  kernel.DrainDeferred();
  const std::vector<std::string> expected = {"stop title threads", "module sweep", "module count 0", "free libraries"};
  CHECK(steps == expected);
  CHECK(kernel.live_libraries() == 0);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

void PatchKeepsTheLoadKey() {
  const std::string patch = ReadFile(std::string(TMNT_PATCH_DIR) + "/0083-recompiled-module-unload-key.patch");
  CHECK(!patch.empty());
  CHECK(Has(patch, "+          module->set_recompiled_key(lib_key);"));
  CHECK(Has(patch, "+    const std::string key = module->recompiled_key();"));
  CHECK(Has(patch, "-    auto recomp = FindRecompiledModule(module->path());"));
  CHECK(!Has(patch, "+    auto recomp = FindRecompiledModule(module->path());"));
}

void PatchKeepsTheExecutableRegion() {
  const std::string patch = ReadFile(std::string(TMNT_PATCH_DIR) + "/0085-xex-load-keeps-heap-regions.patch");
  CHECK(Has(patch, "-  memory()->LookupHeap(base_address_)->Reset();"));
  CHECK(!Has(patch, "+  memory()->LookupHeap(base_address_)->Reset();"));
}

}  // namespace

int main() {
  AbsolutePathNeverMatchesManifestKey();
  StockLookupLeavesTheLibraryLoaded();
  LoadKeyLookupLeavesNoLibraryAfterSweep();
  UnloadedByTheTitleBeforeTheSweep();
  SecondUnloadIsRejected();
  LibraryIsFreedExactlyOnceAndLast();
  DuplicateLoadIsRefused();
  ReloadAfterUnloadWorks();
  ShutdownOrderIsRecorded();
  PatchKeepsTheLoadKey();
  PatchKeepsTheExecutableRegion();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  std::printf(g_failures ? "FAIL\n" : "PASS\n");
  return g_failures ? 1 : 0;
}
