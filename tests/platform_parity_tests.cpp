// Windows and Linux offer the same features. This static test reads the shared manifest that tools/test.ps1 and
// tools/test.sh also read, so the three gates cannot disagree. Headless: no game, window or build is needed.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef TMNT_ROOT_DIR
#error TMNT_ROOT_DIR must name the repository root
#endif

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  PARITY FAIL: %s\n", what.c_str());
  }
}

fs::path Root() { return fs::path(TMNT_ROOT_DIR); }

std::string Read(const std::string& relative) {
  std::ifstream in(Root() / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string out = text.str();
  std::erase(out, '\r');
  return out;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream in(text);
  while (std::getline(in, part, separator)) {
    if (!part.empty()) parts.push_back(part);
  }
  return parts;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

struct Row {
  std::string kind;
  std::string group;
  std::string name;
  std::string path;
  std::vector<std::string> needles;
  std::string extra;
  int line = 0;
};

std::vector<std::string> Groups() {
  std::vector<std::string> groups;
  for (const std::string& line : Lines(Read("tools/test-groups.txt"))) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    groups.push_back(line.substr(first));
  }
  return groups;
}

std::vector<Row> Manifest() {
  std::vector<Row> rows;
  int number = 0;
  for (const std::string& line : Lines(Read("tools/parity-manifest.txt"))) {
    ++number;
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    const std::vector<std::string> fields = Split(line, '\t');
    if (fields.size() != 5 && fields.size() != 6) {
      rows.push_back({.kind = "invalid", .line = number});
      continue;
    }
    Row row;
    row.kind = fields[0];
    row.group = fields[1];
    row.name = fields[2];
    row.path = fields[3];
    row.needles = Split(fields[4], ';');
    row.extra = fields.size() == 6 ? fields[5] : "";
    row.line = number;
    rows.push_back(std::move(row));
  }
  return rows;
}

// Splits the text by the Windows-only #if branches: `keep_windows` selects which side each branch yields.
std::string WithoutWindowsBranches(const std::string& text, bool keep_windows) {
  std::string out;
  struct Frame {
    bool windows;
    bool else_windows;
    bool in_else;
  };
  std::vector<Frame> stack;
  for (const std::string& line : Lines(text)) {
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '#') {
      const std::string directive = line.substr(first + 1);
      const std::string word = directive.substr(0, directive.find_first_of(" \t"));
      std::string compact = directive;
      std::erase(compact, ' ');
      std::erase(compact, '\t');
      if (word == "if" || word == "ifdef" || word == "ifndef") {
        bool windows = false;
        bool else_windows = false;
        if (Has(compact, "ifdef_WIN32")) {
          windows = true;
        } else if (Has(compact, "ifndef_WIN32")) {
          else_windows = true;
        } else if (Has(compact, "ifdef___linux__") || Has(compact, "ifdef___unix__") ||
                   Has(compact, "ifdef___APPLE__")) {
          else_windows = true;
        } else if (Has(compact, "_WIN32") || Has(compact, "defined(WIN32)")) {
          const bool negated = Has(compact, "!defined(_WIN32)") || Has(compact, "!defined(WIN32)");
          windows = !negated;
          else_windows = negated;
        } else if (Has(compact, "__linux__") || Has(compact, "__unix__") || Has(compact, "__APPLE__")) {
          else_windows = true;
        }
        stack.push_back({windows, else_windows, false});
        continue;
      }
      if ((word == "else" || word == "elif") && !stack.empty()) {
        stack.back().in_else = true;
        continue;
      }
      if (word == "endif" && !stack.empty()) {
        stack.pop_back();
        continue;
      }
    }
    const bool in_windows = std::any_of(stack.begin(), stack.end(), [](const Frame& frame) {
      return (frame.windows && !frame.in_else) || (frame.else_windows && frame.in_else);
    });
    if (in_windows == keep_windows) out += line + "\n";
  }
  return out;
}

std::string NonWindows(const std::string& text) { return WithoutWindowsBranches(text, false); }

struct CmakeBranches {
  std::string windows;
  std::string others;
};

CmakeBranches SplitCmakeBranches(const std::string& text) {
  CmakeBranches out;
  struct Frame {
    bool windows;
    bool in_else;
  };
  std::vector<Frame> stack;
  for (const std::string& line : Lines(text)) {
    const size_t first = line.find_first_not_of(" \t");
    std::string low = first == std::string::npos ? std::string() : line.substr(first);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if ((low.starts_with("if(") || low.starts_with("if (")) && low.find(')') != std::string::npos) {
      std::string inner = low.substr(low.find('(') + 1, low.rfind(')') - low.find('(') - 1);
      std::erase(inner, ' ');
      const bool not_windows = Has(inner, "notwin32") || Has(inner, "!win32");
      stack.push_back({Has(inner, "win32") && !not_windows, false});
      continue;
    }
    if (low.starts_with("else(") || low.starts_with("elseif(")) {
      if (!stack.empty()) stack.back().in_else = true;
      continue;
    }
    if (low.starts_with("endif(")) {
      if (!stack.empty()) stack.pop_back();
      continue;
    }
    if (std::any_of(stack.begin(), stack.end(), [](const Frame& frame) { return frame.windows && !frame.in_else; })) {
      out.windows += line + "\n";
    } else {
      out.others += line + "\n";
    }
  }
  return out;
}

void TestManifestMatchesGroupList() {
  const std::vector<std::string> groups = Groups();
  const std::vector<Row> rows = Manifest();
  Check(groups.size() >= 17, "tools/test-groups.txt lists too few groups");
  for (const Row& row : rows) {
    if (row.kind == "invalid") {
      Check(false, "tools/parity-manifest.txt line " + std::to_string(row.line) + " is not six tab-separated columns");
      continue;
    }
    Check(std::find(groups.begin(), groups.end(), row.group) != groups.end(),
          "tools/parity-manifest.txt line " + std::to_string(row.line) + " names unknown group " + row.group);
  }
  for (const std::string& group : groups) {
    const bool used = std::any_of(rows.begin(), rows.end(), [&](const Row& row) { return row.group == group; });
    Check(used, "group " + group + " has no check in tools/parity-manifest.txt");
  }
}

void TestEverySourceIsBuilt() {
  const std::string cmake = Read("CMakeLists.txt");
  int sources = 0;
  for (const auto& entry : fs::recursive_directory_iterator(Root() / "src")) {
    if (!entry.is_regular_file()) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".c") continue;
    ++sources;
    const std::string relative = fs::relative(entry.path(), Root()).generic_string();
    Check(Has(cmake, relative), relative + " is not built by any target");
  }
  Check(sources > 40, "the source scan saw too few files");
}

void TestNoSettingOrHookIsPlatformOnly() {
  int features = 0;
  for (const auto& entry : fs::recursive_directory_iterator(Root() / "src")) {
    if (!entry.is_regular_file()) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".c") continue;
    const std::string relative = fs::relative(entry.path(), Root()).generic_string();
    struct Frame {
      bool windows;
      bool else_windows;
      bool in_else;
    };
    std::vector<Frame> stack;
    int number = 0;
    for (const std::string& line : Lines(Read(relative))) {
      ++number;
      const size_t first = line.find_first_not_of(" \t");
      const std::string code = first == std::string::npos ? std::string() : line.substr(first);
      if (code.starts_with("#")) {
        const std::string directive = code.substr(1);
        const std::string word = directive.substr(0, directive.find_first_of(" \t"));
        std::string compact = directive;
        std::erase(compact, ' ');
        std::erase(compact, '\t');
        if (word == "if" || word == "ifdef" || word == "ifndef") {
          const bool not_windows = Has(compact, "!defined(_WIN32)") || word == "ifndef";
          stack.push_back({Has(compact, "_WIN32") && !not_windows, not_windows, false});
          continue;
        }
        if ((word == "else" || word == "elif") && !stack.empty()) {
          stack.back().in_else = true;
          continue;
        }
        if (word == "endif" && !stack.empty()) {
          stack.pop_back();
          continue;
        }
      }
      if (!Has(code, "REXCVAR_DEFINE") && !Has(code, "REX_HOOK_RAW(")) continue;
      ++features;
      const bool platform_only = std::any_of(stack.begin(), stack.end(), [](const Frame& frame) {
        return (frame.windows && !frame.in_else) || (frame.else_windows && frame.in_else);
      });
      Check(!platform_only, relative + ":" + std::to_string(number) +
                               " defines a setting or hook inside a platform-only #if");
    }
  }
  Check(features > 20, "the platform-only feature scan saw too few settings and hooks");
}

void TestWindowsOnlyCmakeSourcesHaveCounterparts() {
  const CmakeBranches branches = SplitCmakeBranches(Read("CMakeLists.txt"));
  const std::set<std::string> documented = {
      "src/online/websocket_win.cpp", "src/clipboard_win32.cpp", "src/updater/helper_main.cpp"};
  const std::regex source(R"(src/[A-Za-z0-9_./-]+\.(?:cpp|c))");
  const auto matches = [&](const std::string& text) {
    std::set<std::string> paths;
    for (std::sregex_iterator it(text.begin(), text.end(), source), end; it != end; ++it) paths.insert(it->str());
    return paths;
  };
  const std::set<std::string> windows = matches(branches.windows);
  const std::set<std::string> common = matches(branches.others);
  for (const std::string& path : windows) {
    if (common.count(path) == 1) continue;
    Check(documented.count(path) == 1, path + " is built on Windows only and has no declared counterpart");
  }
  Check(windows.size() >= 2, "the Windows-only CMake source scan saw too few files");
  const std::string cmake = Read("CMakeLists.txt");
  for (const char* counterpart : {"src/online/websocket_posix.cpp", "src/clipboard_sdl.cpp"}) {
    Check(Has(cmake, counterpart), std::string(counterpart) + " is not built");
  }
}

void TestBothGatesReadTheSharedMatrix() {
  for (const char* script : {"tools/test.ps1", "tools/test.sh"}) {
    const std::string text = Read(script);
    Check(Has(text, "parity_gate.py") && Has(text, "test-groups.txt"),
          std::string(script) + " does not read the shared test groups and feature matrix");
  }
}

void TestManifestRows() {
  const std::string cmake = Read("CMakeLists.txt");
  for (const Row& row : Manifest()) {
    if (row.kind == "invalid") continue;
    if (row.kind == "pending") {
      if (!row.extra.empty()) {
        Check(!Has(cmake, row.extra), row.name + " is now in the build: turn its pending row into a feature row");
      }
      continue;
    }
    const fs::path path = Root() / row.path;
    Check(fs::exists(path), row.name + ": " + row.path + " does not exist");
    if (!fs::exists(path)) continue;
    if (row.kind == "pair") {
      Check(fs::exists(Root() / row.extra), row.name + ": " + row.extra + " does not exist");
      Check(Has(cmake, row.path) && Has(cmake, row.extra), row.name + ": one platform's source is not built");
      continue;
    }
    if (row.kind == "test") {
      Check(Has(cmake, "add_executable(" + row.extra) || Has(cmake, row.extra),
            row.name + ": CMake target " + row.extra + " is not registered");
      continue;
    }
    if (row.kind == "feature" && path.extension() != ".h" && !Has(cmake, row.path)) {
      Check(false, row.name + ": " + row.path + " is not compiled on both platforms");
      continue;
    }
    std::string text;
    if (fs::is_directory(path)) {
      for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_regular_file()) text += entry.path().filename().string() + "\n";
      }
    } else {
      text = row.kind == "present" ? Read(row.path) : NonWindows(Read(row.path));
    }
    for (const std::string& needle : row.needles) {
      Check(Has(text, needle), row.name + ": " + row.path + " lacks '" + needle + "' outside Windows-only code");
    }
  }
}

void TestLauncherPages() {
  const std::string launcher = NonWindows(Read("src/launcher.cpp"));
  size_t last = 0;
  for (const char* page : {"kPlay", "kDisplay", "kGraphics", "kGameplay", "kSnes", "kCheats", "kOnline",
                           "kAchievements", "kControls", "kBugReport", "kAbout"}) {
    const std::string call = std::string("MakePage(LauncherPage::") + page;
    const size_t at = launcher.find(call);
    Check(at != std::string::npos, std::string("the launcher lost page ") + page);
    if (at != std::string::npos) {
      Check(at >= last, std::string("the launcher moved page ") + page + " out of order");
      last = at;
    }
  }
  Check(Has(launcher, "c.pages.insert(c.pages.begin() + 1, MakePage(LauncherPage::kDlc"),
        "the DLC page no longer inserts after Play");
}

void TestUpdaterAndProtocolAreShared() {
  const std::string config = Read("src/update_config.cpp");
  Check(Has(config, "UpdatePackageKind::kZip"), "the Windows updater no longer installs the release ZIP");
  Check(Has(config, "UpdatePackageKind::kAppImage"), "the Linux updater no longer installs the AppImage");
  const std::string protocol = Read("src/online/protocol.cpp");
  Check(Has(protocol, "protocol_version"), "the shared online protocol lost its version field");
  Check(!Has(protocol, "_WIN32") && !Has(protocol, "__linux__"), "the online protocol became platform-specific");
}

void TestLinuxContracts() {
  struct Contract {
    const char* name;
    const char* path;
    const char* needle;
  };
  const Contract contracts[] = {
      {"user_data follows the AppImage", "src/platform.cpp", "getenv(\"APPIMAGE\")"},
      {"logs live in user_data", "src/log_path.cpp", "user_data_root / \"logs\""},
      {"relaunch restarts the AppImage", "src/platform.cpp", "posix_spawn"},
      {"crash handlers are installed", "src/crash_report.cpp", "InstallPosixHandlers"},
      {"the updater replaces the AppImage", "src/updater/update_platform.cpp", "ApplyAppImageUpdate"},
      {"Bug Report says Linux", "src/incident_report.cpp", "\\\"platform\\\":\\\"Linux\\\""},
      {"the launcher has every page", "src/launcher.cpp", "LauncherPage::kBugReport"},
  };
  for (const Contract& contract : contracts) {
    Check(Has(NonWindows(Read(contract.path)), contract.needle),
          std::string(contract.name) + ": " + contract.path + " lacks " + contract.needle);
  }
}

}  // namespace

int main() {
  TestManifestMatchesGroupList();
  TestEverySourceIsBuilt();
  TestNoSettingOrHookIsPlatformOnly();
  TestWindowsOnlyCmakeSourcesHaveCounterparts();
  TestBothGatesReadTheSharedMatrix();
  TestManifestRows();
  TestLauncherPages();
  TestUpdaterAndProtocolAreShared();
  TestLinuxContracts();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
