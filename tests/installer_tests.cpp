// Tests for the package installer: STFS parsing, path safety, staging and the shared game check.
// Run: tmnt_installer_tests [--scratch DIR] [--package FILE [--reference DIR]]
// With --package, one extraction of the real package is made into the scratch folder and checked.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "game_files.h"
#include "launcher.h"
#include "stfs.h"

namespace fs = std::filesystem;
using namespace tmnt;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      ++g_failures;                                                      \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                    \
  } while (0)

void Section(const char* name) {
  std::printf("%s\n", name);
}

using Bytes = std::vector<uint8_t>;

constexpr uint64_t kBase = 0xB000;
constexpr uint64_t kBlock = 0x1000;

struct Item {
  std::string name;
  int parent = -1;
  bool dir = false;
  Bytes data;
};

Item File(std::string name, int parent, Bytes data) {
  return {std::move(name), parent, false, std::move(data)};
}

Item Dir(std::string name, int parent) {
  return {std::move(name), parent, true, {}};
}

struct BuildOptions {
  const char* magic = "LIVE";
  uint32_t title = kTitleId;
  uint32_t content_type = kGameContentType;
  std::array<uint8_t, 20> content_id = {};
  std::u16string display_name;
  uint32_t license_bits = 0;
  uint32_t license_flags = 0;
  bool read_only = true;
  size_t truncate = 0;
  bool consecutive = true;
};

void PutBe32(Bytes& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24);
  b[at + 1] = uint8_t(v >> 16);
  b[at + 2] = uint8_t(v >> 8);
  b[at + 3] = uint8_t(v);
}

void PutLe24(Bytes& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
  b[at + 2] = uint8_t(v >> 16);
}

uint64_t Physical(uint64_t block) {
  uint64_t index = block + block / 0xAA + 1;
  if (block >= 0xAA) {
    index += 1;
  }
  return kBase + index * kBlock;
}

// A read-only STFS package with the file table in block 0 and consecutive files after it.
Bytes BuildPackage(const std::vector<Item>& items, const BuildOptions& options = {}) {
  uint64_t next_block = 1;
  std::vector<uint64_t> start(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    start[i] = next_block;
    next_block += (items[i].data.size() + kBlock - 1) / kBlock;
  }
  Bytes image(Physical(next_block - 1) + kBlock);
  std::memcpy(image.data(), options.magic, 4);
  PutBe32(image, 0x340, 0xAD0E);
  PutBe32(image, 0x344, options.content_type);
  std::memcpy(&image[0x32C], options.content_id.data(), options.content_id.size());
  for (size_t i = 0; i < options.display_name.size() && i < 127; ++i) {
    image[0x411 + 2 * i] = uint8_t(options.display_name[i] >> 8);
    image[0x412 + 2 * i] = uint8_t(options.display_name[i]);
  }
  PutBe32(image, 0x22C + 8, options.license_bits);
  PutBe32(image, 0x22C + 12, options.license_flags);
  PutBe32(image, 0x360, options.title);
  image[0x379] = 0x24;
  image[0x37B] = options.read_only ? 1 : 0;
  image[0x37C] = 1;
  PutLe24(image, 0x37E, 0);
  for (size_t i = 0; i < items.size(); ++i) {
    uint8_t* e = &image[Physical(0) + i * 0x40];
    std::memcpy(e, items[i].name.data(), items[i].name.size());
    const uint32_t blocks = uint32_t((items[i].data.size() + kBlock - 1) / kBlock);
    e[0x28] = uint8_t(items[i].name.size() | (options.consecutive ? 0x40 : 0) | (items[i].dir ? 0x80 : 0));
    PutLe24(image, i * 0x40 + Physical(0) + 0x29, blocks);
    PutLe24(image, i * 0x40 + Physical(0) + 0x2C, blocks);
    PutLe24(image, i * 0x40 + Physical(0) + 0x2F, uint32_t(start[i]));
    const uint16_t parent = items[i].parent < 0 ? 0xFFFF : uint16_t(items[i].parent);
    e[0x32] = uint8_t(parent >> 8);
    e[0x33] = uint8_t(parent);
    PutBe32(image, i * 0x40 + Physical(0) + 0x34, uint32_t(items[i].data.size()));
    if (!items[i].data.empty()) {
      for (uint64_t b = 0; b * kBlock < items[i].data.size(); ++b) {
        const size_t n = std::min<size_t>(kBlock, items[i].data.size() - b * kBlock);
        std::memcpy(&image[Physical(start[i] + b)], &items[i].data[b * kBlock], n);
      }
    }
  }
  image.resize(image.size() - options.truncate);
  return image;
}

void WriteFile(const fs::path& path, const Bytes& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

Bytes ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Bytes Pattern(size_t size, uint32_t seed) {
  Bytes data(size);
  uint32_t x = seed * 2654435761u + 1;
  for (uint8_t& b : data) {
    x = x * 1664525u + 1013904223u;
    b = uint8_t(x >> 24);
  }
  return data;
}

std::array<uint32_t, 256> CrcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    t[i] = c;
  }
  return t;
}

// A file of the given size and CRC32 that is not the game's file: the last four bytes are chosen to land on
// the target checksum.
Bytes ForgedFile(uint64_t size, uint32_t crc32, uint32_t seed) {
  const auto table = CrcTable();
  Bytes data = Pattern(size_t(size), seed);
  uint32_t reg = 0xFFFFFFFFu;
  for (size_t i = 0; i + 4 < data.size(); ++i) {
    reg = table[(reg ^ data[i]) & 0xFF] ^ (reg >> 8);
  }
  uint8_t idx[4];
  uint32_t t = ~crc32;
  for (int i = 3; i >= 0; --i) {
    int j = 0;
    while ((table[size_t(j)] >> 24) != (t >> 24)) {
      ++j;
    }
    idx[i] = uint8_t(j);
    t = (t ^ table[size_t(j)]) << 8;
  }
  for (int i = 0; i < 4; ++i) {
    data[data.size() - 4 + size_t(i)] = uint8_t((reg ^ idx[i]) & 0xFF);
    reg = table[idx[i]] ^ (reg >> 8);
  }
  return data;
}

Bytes ForgedXex() {
  return ForgedFile(kXexSize, kXexCrc32, 7);
}

Bytes ForgedModuleXex() {
  return ForgedFile(kModuleXexSize, kModuleXexCrc32, 8);
}

std::vector<Item> GamePackageItems(const Bytes& xex, const Bytes& module_xex) {
  return {
      Dir("Data", -1),                                 // 0
      Dir("Sound", 0),                                 // 1
      File("bgm.bnk", 1, Pattern(10000, 1)),           // 2
      File("default.xex", -1, xex),                    // 3
      File(kModuleXexName, -1, module_xex),            // 4
      File("ArcadeInfo.xml", -1, Pattern(3238, 2)),    // 5
      File("empty.txt", 0, {}),                        // 6
      File("big.bin", 0, Pattern(150 * 4096 + 17, 3)), // 7
  };
}

std::vector<Item> ValidGamePackageItems() {
  return GamePackageItems(ForgedXex(), ForgedModuleXex());
}

// Scratch folder removed on destruction.
class Scratch {
 public:
  explicit Scratch(const fs::path& base) {
    static int counter = 0;
    path_ = base / ("case" + std::to_string(++counter));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::set<std::string> Listing(const fs::path& root) {
  std::set<std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    out.insert(fs::relative(e.path(), root).generic_string());
  }
  return out;
}

fs::path g_scratch_base;

// An installed game with a marker, so changes to it are visible.
void MakeOldGame(const fs::path& game) {
  WriteFile(game / "default.xex", Bytes{1, 2, 3});
  WriteFile(game / "Data" / "old.bin", Bytes{9, 9, 9});
  WriteFile(game / "marker.txt", Bytes{'o', 'l', 'd'});
}

bool OldGameIntact(const fs::path& game) {
  return ReadFile(game / "marker.txt") == Bytes{'o', 'l', 'd'} &&
         ReadFile(game / "Data" / "old.bin") == Bytes{9, 9, 9} &&
         ReadFile(game / "default.xex") == Bytes{1, 2, 3};
}

bool NoInstallLeftovers(const fs::path& parent) {
  return !fs::exists(parent / "game-install-temp") && !fs::exists(parent / "game-install-backup");
}

void TestHeader() {
  Section("valid STFS headers are recognized");
  Scratch scratch(g_scratch_base);
  for (const char* magic : {"LIVE", "PIRS", "CON "}) {
    BuildOptions options;
    options.magic = magic;
    WriteFile(scratch.path() / "p.bin", BuildPackage({File("a.txt", -1, Pattern(100, 1))}, options));
    stfs::Package pkg;
    CHECK(pkg.Open(scratch.path() / "p.bin").ok());
    CHECK(pkg.title_id() == kTitleId);
    CHECK(pkg.LoadFiles().ok());
    CHECK(pkg.file_count() == 1);
    CHECK(pkg.total_bytes() == 100);
  }
}

void TestNotPackage() {
  Section("non-STFS input is rejected");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  MakeOldGame(game);
  WriteFile(scratch.path() / "garbage.bin", Pattern(70000, 5));
  WriteFile(scratch.path() / "tiny.bin", Pattern(100, 5));
  WriteFile(scratch.path() / "empty.bin", {});
  for (const char* name : {"garbage.bin", "tiny.bin", "empty.bin"}) {
    stfs::Package pkg;
    CHECK(pkg.Open(scratch.path() / name).status == stfs::Status::kNotPackage);
    CHECK(InstallFromPackage(scratch.path() / name, game).status == InstallStatus::kNotPackage);
  }
  CHECK(InstallFromPackage(scratch.path() / "missing.bin", game).status == InstallStatus::kOpenFailed);
  CHECK(InstallFromPackage(scratch.path(), game).status == InstallStatus::kOpenFailed);
  CHECK(OldGameIntact(game));
  CHECK(NoInstallLeftovers(scratch.path()));
}

void TestWrongTitle() {
  Section("wrong Title ID is rejected before anything is written");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  MakeOldGame(game);
  BuildOptions options;
  options.title = 0x584109E2;
  WriteFile(scratch.path() / "p.bin", BuildPackage(GamePackageItems(Pattern(500, 1), Pattern(400, 2)), options));
  const InstallResult result = InstallFromPackage(scratch.path() / "p.bin", game);
  CHECK(result.status == InstallStatus::kWrongTitle);
  CHECK(result.title_id == 0x584109E2);
  CHECK(OldGameIntact(game));
  CHECK(NoInstallLeftovers(scratch.path()));
}

void TestUnsafeNames() {
  Section("unsafe entry names are rejected");
  const char* names[] = {"..",      ".",         "a\\b",   "a/b",    "C:evil",   "C:\\evil", "\\\\srv\\share",
                         "/abs",    "NUL",       "com1.txt", "con",  "Aux.dat",  "file.",    "file ",
                         "x:stream", "a*b",      "a?b",    "a\"b",   "a<b",      "a|b",      "\001ctl",
                         "caf\xE9", "CONOUT$",   "lpt9"};
  for (const char* name : names) {
    Scratch scratch(g_scratch_base);
    WriteFile(scratch.path() / "p.bin", BuildPackage({File("ok.txt", -1, Pattern(10, 1)), File(name, -1, Pattern(10, 2))}));
    stfs::Package pkg;
    CHECK(pkg.Open(scratch.path() / "p.bin").ok());
    const stfs::Result loaded = pkg.LoadFiles();
    if (loaded.status != stfs::Status::kUnsafePath) {
      std::printf("  not rejected: %s\n", name);
    }
    CHECK(loaded.status == stfs::Status::kUnsafePath);
    const fs::path game = scratch.path() / "game";
    CHECK(InstallFromPackage(scratch.path() / "p.bin", game).status == InstallStatus::kDamagedPackage);
    CHECK(Listing(scratch.path()) == std::set<std::string>{"p.bin"});
  }
}

void TestUnsafeStructure() {
  Section("traversal through directory structure is rejected");
  struct Case {
    const char* what;
    std::vector<Item> items;
  };
  std::vector<Case> cases;
  cases.push_back({"parent cycle", {Item{"a", 0, true, {}}}});
  cases.push_back({"parent out of range", {File("a", 40, Pattern(10, 1))}});
  cases.push_back({"parent is a file", {File("f", -1, Pattern(10, 1)), File("a", 0, Pattern(10, 2))}});
  cases.push_back({"parent is an empty slot", {File("a", 5, Pattern(10, 1))}});
  std::vector<Item> deep;
  deep.push_back(Dir("d0", -1));
  for (int i = 1; i < 40; ++i) {
    deep.push_back(Dir("d" + std::to_string(i), i - 1));
  }
  cases.push_back({"nesting too deep", deep});
  for (const Case& c : cases) {
    Scratch scratch(g_scratch_base);
    WriteFile(scratch.path() / "p.bin", BuildPackage(c.items));
    stfs::Package pkg;
    CHECK(pkg.Open(scratch.path() / "p.bin").ok());
    const stfs::Result loaded = pkg.LoadFiles();
    if (loaded.ok()) {
      std::printf("  not rejected: %s\n", c.what);
    }
    CHECK(!loaded.ok());
    CHECK(Listing(scratch.path()) == std::set<std::string>{"p.bin"});
  }
}

void TestUnsupportedLayouts() {
  Section("layouts the reader does not handle are rejected");
  Scratch scratch(g_scratch_base);
  BuildOptions read_write;
  read_write.read_only = false;
  WriteFile(scratch.path() / "rw.bin", BuildPackage({File("a", -1, Pattern(10, 1))}, read_write));
  stfs::Package rw;
  CHECK(rw.Open(scratch.path() / "rw.bin").ok());
  CHECK(rw.LoadFiles().status == stfs::Status::kUnsupported);

  BuildOptions chained;
  chained.consecutive = false;
  WriteFile(scratch.path() / "chain.bin", BuildPackage({File("a", -1, Pattern(3 * 4096, 1))}, chained));
  stfs::Package ch;
  CHECK(ch.Open(scratch.path() / "chain.bin").ok());
  CHECK(ch.LoadFiles().status == stfs::Status::kUnsupported);
}

void TestExtractionStaysInside() {
  Section("extraction stays inside the root and reproduces the contents");
  Scratch scratch(g_scratch_base);
  const Bytes xex = Pattern(5000, 9);
  const Bytes module_xex = Pattern(3000, 12);
  WriteFile(scratch.path() / "p.bin", BuildPackage(GamePackageItems(xex, module_xex)));
  stfs::Package pkg;
  CHECK(pkg.Open(scratch.path() / "p.bin").ok());
  CHECK(pkg.LoadFiles().ok());
  const fs::path root = scratch.path() / "out" / "root";
  fs::create_directories(root);
  CHECK(pkg.ExtractTo(root, {}).ok());
  CHECK(Listing(scratch.path()) == (std::set<std::string>{"p.bin", "out", "out/root", "out/root/Data",
                                                          "out/root/Data/Sound", "out/root/Data/Sound/bgm.bnk",
                                                          "out/root/Data/empty.txt", "out/root/Data/big.bin",
                                                          "out/root/default.xex", "out/root/TMNT_r.xex",
                                                          "out/root/ArcadeInfo.xml"}));
  CHECK(ReadFile(root / "Data" / "Sound" / "bgm.bnk") == Pattern(10000, 1));
  CHECK(ReadFile(root / "Data" / "big.bin") == Pattern(150 * 4096 + 17, 3));
  CHECK(ReadFile(root / "default.xex") == xex);
  CHECK(ReadFile(root / kModuleXexName) == module_xex);
  CHECK(fs::file_size(root / "Data" / "empty.txt") == 0);
}

void TestBlockBoundary() {
  Section("files that cross hash table boundaries extract intact");
  Scratch scratch(g_scratch_base);
  const Bytes big = Pattern(400 * 4096 + 123, 4);
  WriteFile(scratch.path() / "p.bin", BuildPackage({File("small.bin", -1, Pattern(60 * 4096, 1)), File("big.bin", -1, big)}));
  stfs::Package pkg;
  CHECK(pkg.Open(scratch.path() / "p.bin").ok());
  CHECK(pkg.LoadFiles().ok());
  const fs::path root = scratch.path() / "root";
  fs::create_directories(root);
  CHECK(pkg.ExtractTo(root, {}).ok());
  CHECK(ReadFile(root / "big.bin") == big);
  CHECK(ReadFile(root / "small.bin") == Pattern(60 * 4096, 1));
}

void TestTruncatedAndPartial() {
  Section("truncated, cancelled and failed extractions leave the active game alone");
  {
    Scratch scratch(g_scratch_base);
    const fs::path game = scratch.path() / "game";
    MakeOldGame(game);
    BuildOptions options;
    options.truncate = 5000;
    WriteFile(scratch.path() / "p.bin", BuildPackage(GamePackageItems(Pattern(500, 1), Pattern(400, 2)), options));
    CHECK(InstallFromPackage(scratch.path() / "p.bin", game).status == InstallStatus::kDamagedPackage);
    CHECK(OldGameIntact(game));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
  {
    Scratch scratch(g_scratch_base);
    const fs::path game = scratch.path() / "game";
    MakeOldGame(game);
    WriteFile(scratch.path() / "p.bin", BuildPackage(GamePackageItems(Pattern(500, 1), Pattern(400, 2))));
    bool staging_seen = false;
    InstallHooks hooks;
    hooks.progress = [&](uint64_t done, uint64_t) {
      if (done > 0) {
        staging_seen = fs::exists(scratch.path() / "game-install-temp");
        return false;
      }
      return true;
    };
    CHECK(InstallFromPackage(scratch.path() / "p.bin", game, hooks).status == InstallStatus::kCancelled);
    CHECK(staging_seen);
    CHECK(OldGameIntact(game));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
  {
    Scratch scratch(g_scratch_base);
    const fs::path game = scratch.path() / "game";
    MakeOldGame(game);
    // A file named like an earlier directory fails halfway through the extraction.
    WriteFile(scratch.path() / "p.bin", BuildPackage({File("first.bin", -1, Pattern(50000, 1)), Dir("clash", -1),
                                                      File("clash", -1, Pattern(100, 2))}));
    const InstallResult result = InstallFromPackage(scratch.path() / "p.bin", game);
    CHECK(result.status == InstallStatus::kWriteFailed);
    CHECK(OldGameIntact(game));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
}

void TestInvalidXex() {
  Section("an invalid or missing default.xex or TMNT_r.xex is not activated");
  struct Case {
    const char* what;
    std::vector<Item> items;
    InstallStatus expected;
  };
  const Bytes xex = ForgedXex();
  const Bytes module_xex = ForgedModuleXex();
  const std::vector<Case> cases = {
      {"wrong size", GamePackageItems(Pattern(1000, 1), module_xex), InstallStatus::kWrongVersion},
      {"wrong checksum", GamePackageItems(Bytes(size_t(kXexSize), 0), module_xex), InstallStatus::kWrongVersion},
      {"wrong TMNT_r.xex size", GamePackageItems(xex, Pattern(1000, 2)), InstallStatus::kWrongVersion},
      {"wrong TMNT_r.xex checksum", GamePackageItems(xex, Bytes(size_t(kModuleXexSize), 0)),
       InstallStatus::kWrongVersion},
      {"no Data", {File("default.xex", -1, xex), File(kModuleXexName, -1, module_xex)}, InstallStatus::kIncomplete},
      {"no default.xex",
       {Dir("Data", -1), File("a.bin", 0, Pattern(10, 1)), File(kModuleXexName, -1, module_xex)},
       InstallStatus::kIncomplete},
      {"no TMNT_r.xex",
       {Dir("Data", -1), File("a.bin", 0, Pattern(10, 1)), File("default.xex", -1, xex)},
       InstallStatus::kIncomplete},
      {"TMNT_r.xex is a folder",
       {Dir("Data", -1), File("a.bin", 0, Pattern(10, 1)), File("default.xex", -1, xex), Dir(kModuleXexName, -1),
        File("b.bin", 3, Pattern(10, 2))},
       InstallStatus::kIncomplete},
  };
  for (const Case& c : cases) {
    Scratch scratch(g_scratch_base);
    const fs::path game = scratch.path() / "game";
    MakeOldGame(game);
    WriteFile(scratch.path() / "p.bin", BuildPackage(c.items));
    const InstallResult result = InstallFromPackage(scratch.path() / "p.bin", game);
    if (result.status != c.expected) {
      std::printf("  unexpected status for %s\n", c.what);
    }
    CHECK(result.status == c.expected);
    CHECK(OldGameIntact(game));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
}

void TestValidInstallAndReinstall() {
  Section("a valid package installs, passes the shared check, and reinstall keeps user data");
  Scratch scratch(g_scratch_base);
  const Bytes xex = ForgedXex();
  const Bytes module_xex = ForgedModuleXex();
  CHECK(xex.size() == kXexSize);
  CHECK(module_xex.size() == kModuleXexSize);
  WriteFile(scratch.path() / "p.bin", BuildPackage(GamePackageItems(xex, module_xex)));
  const fs::path game = scratch.path() / "program" / "game";

  CHECK(CheckGameFiles(game).state == GameFiles::kMissing);
  int phases = 0;
  InstallHooks hooks;
  hooks.phase = [&](const char*) { ++phases; };
  const InstallResult first = InstallFromPackage(scratch.path() / "p.bin", game, hooks);
  CHECK(first.ok());
  CHECK(first.files == 6);
  CHECK(phases == 4);
  const GameCheck check = CheckGameFiles(game);
  CHECK(check.state == GameFiles::kReady);
  CHECK(check.xex_crc32 == kXexCrc32);
  CHECK(check.module_xex_crc32 == kModuleXexCrc32);
  CHECK(ReadFile(game / "Data" / "Sound" / "bgm.bnk") == Pattern(10000, 1));
  CHECK(NoInstallLeftovers(scratch.path() / "program"));

  const fs::path user = scratch.path() / "Documents" / "tmnt_turtles_in_time_reshelled";
  WriteFile(user / "settings.toml", Bytes{'f', 'u', 'l', 'l', 's', 'c', 'r', 'e', 'e', 'n', '=', 't'});
  WriteFile(user / "cache" / "shader.xsh", Pattern(300, 8));
  WriteFile(user / "content" / "save.bin", Pattern(900, 9));
  WriteFile(scratch.path() / "program" / "logs" / "run.log", Pattern(200, 10));
  WriteFile(game / "stale.txt", Bytes{1});
  const auto before = Listing(scratch.path() / "Documents");

  const InstallResult second = InstallFromPackage(scratch.path() / "p.bin", game);
  CHECK(second.ok());
  CHECK(!fs::exists(game / "stale.txt"));
  CHECK(CheckGameFiles(game).state == GameFiles::kReady);
  CHECK(NoInstallLeftovers(scratch.path() / "program"));
  CHECK(Listing(scratch.path() / "Documents") == before);
  CHECK(ReadFile(user / "settings.toml") == (Bytes{'f', 'u', 'l', 'l', 's', 'c', 'r', 'e', 'e', 'n', '=', 't'}));
  CHECK(ReadFile(user / "cache" / "shader.xsh") == Pattern(300, 8));
  CHECK(ReadFile(user / "content" / "save.bin") == Pattern(900, 9));
  CHECK(ReadFile(scratch.path() / "program" / "logs" / "run.log") == Pattern(200, 10));
}

void TestForeignFolder() {
  Section("a folder of unrelated files is not replaced");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  WriteFile(game / "photos" / "a.jpg", Pattern(100, 1));
  WriteFile(scratch.path() / "p.bin", BuildPackage(ValidGamePackageItems()));
  CHECK(InstallFromPackage(scratch.path() / "p.bin", game).status == InstallStatus::kForeignFolder);
  CHECK(ReadFile(game / "photos" / "a.jpg") == Pattern(100, 1));
  CHECK(NoInstallLeftovers(scratch.path()));
  const fs::path module_only = scratch.path() / "module_only";
  WriteFile(module_only / kModuleXexName, Bytes{1, 2, 3});
  CHECK(InstallFromPackage(scratch.path() / "p.bin", module_only).ok());
  CHECK(CheckGameFiles(module_only).state == GameFiles::kReady);
  CHECK(NoInstallLeftovers(scratch.path()));
  CHECK(InstallFromPackage(scratch.path() / "p.bin", {}).status == InstallStatus::kNoGameFolder);
  CHECK(InstallFromPackage(scratch.path() / "p.bin", scratch.path().root_path()).status ==
        InstallStatus::kNoGameFolder);
}

void TestCaseVariantGameNames() {
  Section("a game folder copied with another letter case is recognised on both platforms");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  WriteFile(game / "Default.xex", Pattern(16, 1));
  WriteFile(game / "tmnt_r.xex", Pattern(16, 2));
  fs::create_directories(game / "data" / "Sound");
  CHECK(CheckGameFiles(game).state == GameFiles::kUnsupported);
  WriteFile(scratch.path() / "p.bin", BuildPackage(ValidGamePackageItems()));
  CHECK(InstallFromPackage(scratch.path() / "p.bin", game).ok());
  CHECK(CheckGameFiles(game).state == GameFiles::kReady);
  CHECK(ReadFile(game / "Data" / "Sound" / "bgm.bnk") == Pattern(10000, 1));
}

void TestLauncherDecision() {
  Section("missing game files force the launcher; the relaunch flag and the off setting cannot hide it");
  for (const bool skip : {false, true}) {
    for (const bool shift : {false, true}) {
      for (const bool enabled : {false, true}) {
        CHECK(LauncherWanted(false, skip, shift, enabled));
      }
    }
  }
  CHECK(!LauncherWanted(true, false, false, false));
  CHECK(LauncherWanted(true, false, true, false));
  CHECK(LauncherWanted(true, false, false, true));
  CHECK(!LauncherWanted(true, true, true, true));
}

void TestOnWorkerThread() {
  Section("the installer runs and cancels on a worker thread");
  Scratch scratch(g_scratch_base);
  WriteFile(scratch.path() / "p.bin", BuildPackage(ValidGamePackageItems()));
  {
    std::atomic<bool> finished{false};
    InstallResult result;
    std::thread worker([&] {
      result = InstallFromPackage(scratch.path() / "p.bin", scratch.path() / "game");
      finished = true;
    });
    while (!finished) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    CHECK(result.ok());
  }
  {
    std::atomic<bool> cancel{false};
    InstallResult result;
    InstallHooks hooks;
    hooks.progress = [&](uint64_t done, uint64_t) {
      if (done > 0) {
        cancel = true;
      }
      return !cancel;
    };
    std::thread worker([&] { result = InstallFromPackage(scratch.path() / "p.bin", scratch.path() / "game2", hooks); });
    worker.join();
    CHECK(result.status == InstallStatus::kCancelled);
    CHECK(!fs::exists(scratch.path() / "game2"));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
}

bool SameFile(const fs::path& a, const fs::path& b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  Bytes ba(1 << 20), bb(1 << 20);
  while (true) {
    fa.read(reinterpret_cast<char*>(ba.data()), std::streamsize(ba.size()));
    fb.read(reinterpret_cast<char*>(bb.data()), std::streamsize(bb.size()));
    if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) {
      return false;
    }
    if (fa.gcount() == 0) {
      return true;
    }
  }
}

const std::array<uint8_t, 20> kAddOnId = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22,
                                          0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
const std::array<uint8_t, 20> kSecondAddOnId = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0x0F, 0x1E,
                                                0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0x87, 0x96, 0xA5, 0xB4};
const char* const kAddOnIdHex = "123456789ABCDEF0112233445566778899AABBCC";
const char* const kSecondAddOnIdHex = "FEDCBA98765432100F1E2D3C4B5A69788796A5B4";

BuildOptions AddOn(const std::array<uint8_t, 20>& id, const std::u16string& name) {
  BuildOptions options;
  options.content_type = kDlcContentType;
  options.content_id = id;
  options.display_name = name;
  options.license_bits = 0x4;
  options.license_flags = 1;
  return options;
}

std::vector<Item> AddOnItems(uint32_t seed) {
  return {File("addon.bin", -1, Pattern(7000, seed)), Dir("extra", -1), File("a.bin", 1, Pattern(300, seed + 1))};
}

fs::path DlcFolderOf(const fs::path& user, const char* content_id) {
  return user / "0000000000000000" / "584109D2" / "00000002" / content_id;
}

fs::path DlcHeaderOf(const fs::path& user, const char* content_id) {
  return user / "0000000000000000" / "584109D2" / "Headers" / "00000002" / (std::string(content_id) + ".header");
}

bool NoDlcLeftovers(const fs::path& user) {
  return !fs::exists(user / "dlc-install-temp");
}

uint32_t ReadBe32At(const Bytes& b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

uint32_t ReadLe32At(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

std::u16string Utf16BeAt(const Bytes& b, size_t at, size_t chars) {
  std::u16string out;
  for (size_t i = 0; i < chars; ++i) {
    const char16_t c = char16_t(b[at + 2 * i] << 8 | b[at + 2 * i + 1]);
    if (c == 0) {
      break;
    }
    out.push_back(c);
  }
  return out;
}

void TestDlcMetadata() {
  Section("DLC metadata: content type, content ID, display name and licence mask are read from the package");
  Scratch scratch(g_scratch_base);
  WriteFile(scratch.path() / "dlc.bin", BuildPackage(AddOnItems(1), AddOn(kAddOnId, u"Test add-on")));
  stfs::Package pkg;
  CHECK(pkg.Open(scratch.path() / "dlc.bin").ok());
  CHECK(pkg.title_id() == kTitleId);
  CHECK(pkg.content_type() == kDlcContentType);
  CHECK(pkg.content_id_hex() == kAddOnIdHex);
  CHECK(pkg.display_name() == u"Test add-on");
  CHECK(pkg.license_mask() == 0x4);
  CHECK(pkg.LoadFiles().ok());
  CHECK(pkg.file_count() == 2);

  WriteFile(scratch.path() / "game.bin", BuildPackage({File("default.xex", -1, Pattern(100, 1))}));
  stfs::Package game;
  CHECK(game.Open(scratch.path() / "game.bin").ok());
  CHECK(game.content_type() == kGameContentType);
  CHECK(game.display_name().empty());
  CHECK(game.license_mask() == 0);
}

void TestDlcRejections() {
  Section("DLC installer rejects the game and other titles, the game installer rejects DLC, nothing is written");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  const fs::path user = scratch.path() / "Documents";
  MakeOldGame(game);
  WriteFile(scratch.path() / "game.bin", BuildPackage({File("default.xex", -1, Pattern(100, 1))}));
  WriteFile(scratch.path() / "dlc.bin", BuildPackage(AddOnItems(1), AddOn(kAddOnId, u"Test add-on")));
  BuildOptions other = AddOn(kAddOnId, u"Other title");
  other.title = 0x584109E2;
  WriteFile(scratch.path() / "other.bin", BuildPackage(AddOnItems(2), other));

  CHECK(InstallDlcFromPackage(scratch.path() / "game.bin", user).status == InstallStatus::kNotDlcPackage);
  CHECK(InstallFromPackage(scratch.path() / "dlc.bin", game).status == InstallStatus::kNotGamePackage);
  const InstallResult wrong = InstallDlcFromPackage(scratch.path() / "other.bin", user);
  CHECK(wrong.status == InstallStatus::kWrongTitle);
  CHECK(wrong.title_id == 0x584109E2);
  CHECK(InstallDlcFromPackage(scratch.path() / "dlc.bin", {}).status == InstallStatus::kNoUserFolder);
  CHECK(!fs::exists(user));
  CHECK(ListInstalledDlc(user).empty());
  CHECK(OldGameIntact(game));
  CHECK(NoInstallLeftovers(scratch.path()));
}

void TestDlcInstallSideBySide() {
  Section("DLC packages install side by side; reinstalling one keeps the others and the game folder");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  const fs::path user = scratch.path() / "Documents";
  MakeOldGame(game);
  WriteFile(scratch.path() / "a.bin", BuildPackage(AddOnItems(1), AddOn(kAddOnId, u"Test add-on")));
  WriteFile(scratch.path() / "b.bin", BuildPackage(AddOnItems(2), AddOn(kSecondAddOnId, u"")));

  DlcInfo first_info;
  const InstallResult first = InstallDlcFromPackage(scratch.path() / "a.bin", user, {}, &first_info);
  CHECK(first.ok());
  CHECK(first.title_id == kTitleId);
  CHECK(first.files == 2);
  CHECK(first_info.content_id == kAddOnIdHex);
  CHECK(first_info.display_name == "Test add-on");
  CHECK(first_info.license_mask == 0x4);
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin") == Pattern(7000, 1));
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "extra" / "a.bin") == Pattern(300, 2));

  const Bytes header = ReadFile(DlcHeaderOf(user, kAddOnIdHex));
  CHECK(header.size() == 0x14C);
  CHECK(ReadBe32At(header, 4) == kDlcContentType);
  CHECK(ReadBe32At(header, 0x140) == kTitleId);
  CHECK(std::string(header.begin() + 264, header.begin() + 264 + 40) == kAddOnIdHex);
  CHECK(header[264 + 40] == 0);
  CHECK(Utf16BeAt(header, 8, 128) == u"Test add-on");
  CHECK(ReadLe32At(header, 0x148) == 0x4);

  CHECK(InstallDlcFromPackage(scratch.path() / "b.bin", user).ok());
  const std::vector<DlcInfo> listed = ListInstalledDlc(user);
  CHECK(listed.size() == 2);
  if (listed.size() == 2) {
    CHECK(listed[0].content_id == kAddOnIdHex);
    CHECK(listed[0].display_name == "Test add-on");
    CHECK(listed[1].content_id == kSecondAddOnIdHex);
    CHECK(listed[1].display_name == kSecondAddOnIdHex);
  }

  const Bytes first_file = ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin");
  CHECK(InstallDlcFromPackage(scratch.path() / "a.bin", user).ok());
  CHECK(ListInstalledDlc(user).size() == 2);
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin") == first_file);
  CHECK(ReadFile(DlcFolderOf(user, kSecondAddOnIdHex) / "addon.bin") == Pattern(7000, 2));
  CHECK(NoDlcLeftovers(user));

  WriteFile(scratch.path() / "game.bin", BuildPackage(ValidGamePackageItems()));
  CHECK(InstallFromPackage(scratch.path() / "game.bin", game).ok());
  CHECK(ListInstalledDlc(user).size() == 2);
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin") == first_file);
  CHECK(NoInstallLeftovers(scratch.path()));
}

void TestDlcFailuresKeepInstalled() {
  Section("failed, cancelled and clashing DLC installs leave the installed DLC as it was");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  WriteFile(scratch.path() / "a.bin", BuildPackage(AddOnItems(1), AddOn(kAddOnId, u"Test add-on")));
  CHECK(InstallDlcFromPackage(scratch.path() / "a.bin", user).ok());
  const Bytes header_before = ReadFile(DlcHeaderOf(user, kAddOnIdHex));
  const Bytes file_before = ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin");
  const auto listing_before = Listing(user);

  BuildOptions truncated = AddOn(kAddOnId, u"Test add-on");
  truncated.truncate = 5000;
  WriteFile(scratch.path() / "cut.bin", BuildPackage(AddOnItems(3), truncated));
  CHECK(InstallDlcFromPackage(scratch.path() / "cut.bin", user).status == InstallStatus::kDamagedPackage);

  WriteFile(scratch.path() / "c.bin", BuildPackage(AddOnItems(4), AddOn(kSecondAddOnId, u"Second")));
  InstallHooks cancel;
  cancel.progress = [](uint64_t done, uint64_t) { return done == 0; };
  CHECK(InstallDlcFromPackage(scratch.path() / "c.bin", user, cancel).status == InstallStatus::kCancelled);

  WriteFile(scratch.path() / "clash.bin", BuildPackage({File("first.bin", -1, Pattern(50000, 1)), Dir("clash", -1),
                                                        File("clash", -1, Pattern(100, 2))},
                                                       AddOn(kSecondAddOnId, u"Clash")));
  CHECK(InstallDlcFromPackage(scratch.path() / "clash.bin", user).status == InstallStatus::kWriteFailed);

  CHECK(ReadFile(DlcHeaderOf(user, kAddOnIdHex)) == header_before);
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin") == file_before);
  CHECK(Listing(user) == listing_before);
  CHECK(ListInstalledDlc(user).size() == 1);
  CHECK(NoDlcLeftovers(user));
}

void TestDlcUnsafeEntries() {
  Section("a DLC package with an unsafe entry name is rejected before anything is written");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  WriteFile(scratch.path() / "evil.bin",
            BuildPackage({File("ok.bin", -1, Pattern(10, 1)), File("..", -1, Pattern(10, 2))}, AddOn(kAddOnId, u"Evil")));
  CHECK(InstallDlcFromPackage(scratch.path() / "evil.bin", user).status == InstallStatus::kDamagedPackage);
  CHECK(!fs::exists(user));
  CHECK(Listing(scratch.path()) == std::set<std::string>{"evil.bin"});
}

std::array<uint8_t, 20> IdFrom(uint8_t seed) {
  std::array<uint8_t, 20> id{};
  for (size_t i = 0; i < id.size(); ++i) {
    id[i] = uint8_t(seed + 37 * i);
  }
  return id;
}

std::string HexOf(const std::array<uint8_t, 20>& id) {
  std::string out;
  char digits[3];
  for (const uint8_t byte : id) {
    std::snprintf(digits, sizeof(digits), "%02X", byte);
    out += digits;
  }
  return out;
}

std::u16string Wide(const char* text) {
  return std::u16string(text, text + std::strlen(text));
}

const char* const kPackageNames[] = {"First add-on", "Second add-on", "Third add-on"};

// Installs three packages under `user`, each with a file that a removal must leave alone when it is not the one
// removed. Returns their content IDs in the order above.
std::array<std::string, 3> InstallPackages(const fs::path& scratch, const fs::path& user) {
  std::array<std::string, 3> ids;
  const std::array<uint8_t, 20> seeds[] = {IdFrom(0x10), IdFrom(0x40), IdFrom(0x70)};
  for (size_t i = 0; i < 3; ++i) {
    const fs::path package = scratch / ("package" + std::to_string(i) + ".bin");
    WriteFile(package, BuildPackage(AddOnItems(uint32_t(10 + i)), AddOn(seeds[i], Wide(kPackageNames[i]))));
    CHECK(InstallDlcFromPackage(package, user).ok());
    ids[i] = HexOf(seeds[i]);
  }
  return ids;
}

// The listing with every entry that names `id` taken out.
std::set<std::string> WithoutDlc(std::set<std::string> listing, const std::string& id) {
  for (auto it = listing.begin(); it != listing.end();) {
    it = it->find(id) != std::string::npos ? listing.erase(it) : std::next(it);
  }
  return listing;
}

void TestDlcListing() {
  Section("installed packages are listed once each by content ID and display name, and removed by that ID");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  MakeOldGame(game);
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);

  const std::vector<DlcInfo> listed = ListInstalledDlc(user);
  CHECK(listed.size() == 3);
  for (size_t i = 0; i < ids.size(); ++i) {
    const auto matches = std::count_if(listed.begin(), listed.end(), [&](const DlcInfo& dlc) {
      return dlc.content_id == ids[i] && dlc.display_name == kPackageNames[i] && dlc.verified;
    });
    CHECK(matches == 1);
  }

  const std::set<std::string> before = Listing(user);
  CHECK(RemoveInstalledDlc(user, game, ids[1]).ok());
  CHECK(Listing(user) == WithoutDlc(before, ids[1]));
  CHECK(ListInstalledDlc(user).size() == 2);
  CHECK(ReadFile(DlcFolderOf(user, ids[0].c_str()) / "addon.bin") == Pattern(7000, 10));

  fs::remove_all(DlcFolderOf(user, ids[0].c_str()));
  const std::vector<DlcInfo> after = ListInstalledDlc(user);
  CHECK(after.size() == 1);
  CHECK(std::none_of(after.begin(), after.end(), [&](const DlcInfo& dlc) { return dlc.content_id == ids[0]; }));
  CHECK(!fs::exists(user / "settings.toml"));
  CHECK(OldGameIntact(game));
}

void TestUninstallEachPackage() {
  Section("removing one DLC package removes only that package; the others, the game and the other folders stay");
  for (size_t target = 0; target < 3; ++target) {
    Scratch scratch(g_scratch_base);
    const fs::path game = scratch.path() / "game";
    const fs::path user = scratch.path() / "Documents";
    MakeOldGame(game);
    const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
    const auto listing_before = Listing(user);
    const auto game_before = Listing(game);

    const RemoveResult result = RemoveInstalledDlc(user, game, ids[target]);
    if (!result.ok()) {
      std::printf("  removal %zu: %s\n", target, result.detail.c_str());
    }
    CHECK(result.ok());
    CHECK(!fs::exists(DlcFolderOf(user, ids[target].c_str())));
    CHECK(!fs::exists(DlcHeaderOf(user, ids[target].c_str())));
    CHECK(Listing(user) == WithoutDlc(listing_before, ids[target]));
    for (size_t other = 0; other < 3; ++other) {
      if (other == target) {
        continue;
      }
      CHECK(ReadFile(DlcFolderOf(user, ids[other].c_str()) / "addon.bin") == Pattern(7000, uint32_t(10 + other)));
      CHECK(fs::exists(DlcHeaderOf(user, ids[other].c_str())));
    }
    CHECK(ListInstalledDlc(user).size() == 2);
    CHECK(Listing(game) == game_before);
    CHECK(OldGameIntact(game));
    CHECK(NoDlcLeftovers(user));
    CHECK(NoInstallLeftovers(scratch.path()));
  }
}

void TestUninstallKeepsUserData() {
  Section("removing DLC leaves saves, settings, the shader cache and other marketplace content alone");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  const Bytes settings{'f', 'u', 'l', 'l', 's', 'c', 'r', 'e', 'e', 'n'};
  WriteFile(user / "settings.toml", settings);
  WriteFile(user / "cache" / "shader.xsh", Pattern(300, 8));
  WriteFile(user / "0123456789ABCDEF" / "584109D2" / "00000001" / "SAVEGAME" / "save.bin", Pattern(900, 9));
  WriteFile(user / "0123456789ABCDEF" / "584109D2" / "Headers" / "00000001" / "SAVEGAME.header", Pattern(0x148, 4));
  WriteFile(user / "0000000000000000" / "584109E2" / "00000002" / "OTHER" / "data.bin", Pattern(100, 5));
  WriteFile(user / "0000000000000000" / "584109E2" / "Headers" / "00000002" / "OTHER.header", Pattern(0x148, 6));
  const auto listing_before = Listing(user);

  CHECK(RemoveInstalledDlc(user, scratch.path() / "game", ids[0]).ok());
  CHECK(ReadFile(user / "settings.toml") == settings);
  CHECK(ReadFile(user / "cache" / "shader.xsh") == Pattern(300, 8));
  CHECK(ReadFile(user / "0123456789ABCDEF" / "584109D2" / "00000001" / "SAVEGAME" / "save.bin") == Pattern(900, 9));
  CHECK(ReadFile(user / "0000000000000000" / "584109E2" / "00000002" / "OTHER" / "data.bin") == Pattern(100, 5));
  CHECK(Listing(user) == WithoutDlc(listing_before, ids[0]));
}

void TestUninstallRejectsBadTargets() {
  Section("absent, empty, malformed and traversal content IDs are refused and nothing else is touched");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  MakeOldGame(game);
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  WriteFile(user / "keep.txt", Bytes{'k'});
  WriteFile(user / "0000000000000000" / "584109D2" / "keep.txt", Bytes{'k'});
  WriteFile(user / "0000000000000000" / "584109D2" / "Headers" / "keep.txt", Bytes{'k'});
  const auto listing_before = Listing(user);

  CHECK(RemoveInstalledDlc(user, game, HexOf(IdFrom(0xA0))).status == RemoveStatus::kNotInstalled);
  CHECK(RemoveInstalledDlc({}, game, HexOf(IdFrom(0xA0))).status == RemoveStatus::kNotInstalled);

  std::string lower = ids[0];
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
  const std::string malformed[] = {"",
                                   ".",
                                   "..",
                                   "../..",
                                   "..\\..\\keep.txt",
                                   "C:\\Windows",
                                   "/etc/passwd",
                                   "\\\\server\\share",
                                   ids[0].substr(0, 39),
                                   ids[0] + "0",
                                   ids[0] + "/..",
                                   lower,
                                   std::string("G") + ids[0].substr(1),
                                   std::string(40, '\0')};
  for (const std::string& id : malformed) {
    const RemoveResult result = RemoveInstalledDlc(user, game, id);
    if (result.status != RemoveStatus::kUnsafeTarget) {
      std::printf("  not refused: a %zu-byte content ID\n", id.size());
    }
    CHECK(result.status == RemoveStatus::kUnsafeTarget);
  }
  CHECK(Listing(user) == listing_before);
  CHECK(fs::is_directory(user / "0000000000000000" / "584109D2" / "00000002"));
  CHECK(ListInstalledDlc(user).size() == 3);
  CHECK(OldGameIntact(game));
}

void TestFailedUninstallIsNotSuccess() {
  Section("a header that cannot be deleted puts the DLC back and is reported as a failure");
#if defined(_WIN32)
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  const auto listing_before = Listing(user);
  {
    std::ifstream held(DlcHeaderOf(user, ids[0].c_str()), std::ios::binary);
    const RemoveResult result = RemoveInstalledDlc(user, game, ids[0]);
    CHECK(result.status == RemoveStatus::kWriteFailed);
    CHECK(!result.ok());
    CHECK(Listing(user) == listing_before);
    CHECK(ListInstalledDlc(user).size() == 3);
    CHECK(ReadFile(DlcFolderOf(user, ids[0].c_str()) / "addon.bin") == Pattern(7000, 10));
  }
  CHECK(RemoveInstalledDlc(user, game, ids[0]).ok());
  CHECK(ListInstalledDlc(user).size() == 2);
  CHECK(NoDlcLeftovers(user));
#else
  std::printf("  skipped: the header lock applies on Windows only\n");
#endif
}

// A directory link to `target`: a symbolic link where the system allows one, else a junction, which needs no privilege.
bool MakeDirectoryLink(const fs::path& target, const fs::path& link) {
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  if (!ec) {
    return true;
  }
#if defined(_WIN32)
  const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" > nul";
  return std::system(command.c_str()) == 0;
#else
  return false;
#endif
}

void TestUninstallRefusesLinks() {
  Section("a link inside a DLC folder, or in place of one, stops the removal before anything is moved");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  const fs::path outside = scratch.path() / "outside";
  WriteFile(outside / "keep.bin", Pattern(100, 3));
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  const std::string target = ids[0];
  const fs::path folder = DlcFolderOf(user, target.c_str());
  const fs::path header = DlcHeaderOf(user, target.c_str());

  if (!MakeDirectoryLink(outside, folder / "linked")) {
    std::printf("  skipped: no symbolic link or junction can be created here\n");
    return;
  }
  CHECK(RemoveInstalledDlc(user, game, target).status == RemoveStatus::kUnsafeTarget);
  CHECK(ReadFile(outside / "keep.bin") == Pattern(100, 3));
  CHECK(ReadFile(folder / "addon.bin") == Pattern(7000, 10));
  std::error_code ec;
  fs::remove(folder / "linked", ec);
  CHECK(ReadFile(outside / "keep.bin") == Pattern(100, 3));

  const fs::path moved = scratch.path() / "moved";
  fs::rename(folder, moved, ec);
  CHECK(!ec);
  if (MakeDirectoryLink(outside, folder)) {
    CHECK(RemoveInstalledDlc(user, game, target).status == RemoveStatus::kUnsafeTarget);
    CHECK(ReadFile(outside / "keep.bin") == Pattern(100, 3));
    CHECK(fs::exists(folder));
    fs::remove(folder, ec);
    CHECK(ReadFile(outside / "keep.bin") == Pattern(100, 3));
  }
  fs::rename(moved, folder, ec);
  CHECK(ReadFile(folder / "addon.bin") == Pattern(7000, 10));

  const fs::path decoy = scratch.path() / "decoy";
  WriteFile(decoy / "keep.bin", Pattern(40, 4));
  const fs::path work = user / "dlc-remove-temp";
  if (MakeDirectoryLink(decoy, work)) {
    CHECK(RemoveInstalledDlc(user, game, target).status == RemoveStatus::kUnsafeTarget);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
    CHECK(ReadFile(folder / "addon.bin") == Pattern(7000, 10));
    fs::remove(work, ec);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
  }

  const fs::path gone = scratch.path() / "gone";
  fs::create_directories(gone);
  if (MakeDirectoryLink(gone, work)) {
    fs::remove(gone, ec);
    CHECK(RemoveInstalledDlc(user, game, target).status == RemoveStatus::kUnsafeTarget);
    CHECK(!fs::exists(gone));
    CHECK(ReadFile(folder / "addon.bin") == Pattern(7000, 10));
    fs::remove(work, ec);
  }

  fs::remove(header, ec);
  fs::create_symlink(outside / "keep.bin", header, ec);
  if (!ec) {
    CHECK(RemoveInstalledDlc(user, game, target).status == RemoveStatus::kUnverified);
    CHECK(ReadFile(outside / "keep.bin") == Pattern(100, 3));
  } else {
    std::printf("  skipped: a file symbolic link cannot be created here\n");
  }
}

void TestDlcWorkFolderCleanup() {
  Section("the DLC work folder is cleared before and after an install, whether it succeeds or is cancelled");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  WriteFile(scratch.path() / "a.bin", BuildPackage(AddOnItems(1), AddOn(kAddOnId, u"Test add-on")));
  WriteFile(user / "dlc-install-temp" / "staged" / "files" / "left.bin", Pattern(64, 5));
  WriteFile(user / "dlc-install-temp" / "backup" / "left.bin", Pattern(64, 6));
  CHECK(InstallDlcFromPackage(scratch.path() / "a.bin", user).ok());
  CHECK(NoDlcLeftovers(user));
  CHECK(ReadFile(DlcFolderOf(user, kAddOnIdHex) / "addon.bin") == Pattern(7000, 1));

  WriteFile(scratch.path() / "b.bin", BuildPackage(AddOnItems(2), AddOn(kSecondAddOnId, u"Second")));
  InstallHooks cancel;
  cancel.progress = [](uint64_t done, uint64_t) { return done == 0; };
  CHECK(InstallDlcFromPackage(scratch.path() / "b.bin", user, cancel).status == InstallStatus::kCancelled);
  CHECK(NoDlcLeftovers(user));
  CHECK(ListInstalledDlc(user).size() == 1);
}

void TestDlcInstallRefusesLinks() {
  Section("a DLC install stops when its work folder or the installed copy holds a link; nothing else changes");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  const fs::path decoy = scratch.path() / "decoy";
  const fs::path outside = scratch.path() / "outside";
  const fs::path first_package = scratch.path() / "package0.bin";
  MakeOldGame(game);
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  const fs::path first_folder = DlcFolderOf(user, ids[0].c_str());
  const fs::path work = user / "dlc-install-temp";
  const std::set<std::string> game_before = Listing(game);
  WriteFile(decoy / "keep.bin", Pattern(40, 4));
  std::error_code ec;

  if (MakeDirectoryLink(decoy, work)) {
    const std::set<std::string> before = Listing(user);
    const InstallResult result = InstallDlcFromPackage(first_package, user);
    CHECK(result.status == InstallStatus::kUnsafeTarget);
    CHECK(std::string(DescribeInstall(result.status).title) == "Could not prepare the DLC install folder");
    CHECK(Listing(user) == before);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
    fs::remove(work, ec);
  } else {
    std::printf("  skipped: no symbolic link or junction can be created here\n");
  }

  fs::create_directories(work / "backup");
  if (MakeDirectoryLink(decoy, work / "backup" / "folder")) {
    const std::set<std::string> before = Listing(user);
    CHECK(InstallDlcFromPackage(first_package, user).status == InstallStatus::kUnsafeTarget);
    CHECK(Listing(user) == before);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
    fs::remove(work / "backup" / "folder", ec);
  }
  fs::remove_all(work, ec);

  fs::create_directories(work);
  ec.clear();
  fs::create_symlink(decoy / "keep.bin", work / "note.bin", ec);
  if (!ec) {
    CHECK(InstallDlcFromPackage(first_package, user).status == InstallStatus::kUnsafeTarget);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
    fs::remove(work / "note.bin", ec);
  } else {
    std::printf("  skipped: a file symbolic link cannot be created here\n");
  }
  fs::remove_all(work, ec);

  WriteFile(work, Pattern(10, 6));
  CHECK(InstallDlcFromPackage(first_package, user).status == InstallStatus::kUnsafeTarget);
  CHECK(ReadFile(work) == Pattern(10, 6));
  fs::remove(work, ec);

  const fs::path missing = outside / "missing";
  fs::create_directories(missing);
  if (MakeDirectoryLink(missing, work)) {
    fs::remove(missing, ec);
    CHECK(InstallDlcFromPackage(first_package, user).status == InstallStatus::kUnsafeTarget);
    CHECK(!fs::exists(missing));
    fs::remove(work, ec);
  }

  if (MakeDirectoryLink(decoy, first_folder / "linked")) {
    const std::set<std::string> before = Listing(user);
    CHECK(InstallDlcFromPackage(first_package, user).status == InstallStatus::kUnsafeTarget);
    CHECK(Listing(user) == before);
    CHECK(ReadFile(decoy / "keep.bin") == Pattern(40, 4));
    CHECK(ReadFile(first_folder / "addon.bin") == Pattern(7000, 10));
    CHECK(NoDlcLeftovers(user));
    CHECK(ListInstalledDlc(user).size() == 3);
    fs::remove(first_folder / "linked", ec);
  }

  CHECK(InstallDlcFromPackage(first_package, user).ok());
  CHECK(NoDlcLeftovers(user));
  CHECK(ReadFile(first_folder / "addon.bin") == Pattern(7000, 10));
  CHECK(ListInstalledDlc(user).size() == 3);
  CHECK(OldGameIntact(game));
  CHECK(Listing(game) == game_before);
}

void TestUninstallRefusesGameFolderInside() {
  Section("a game folder inside the DLC folder stops the removal, so the game is never deleted with it");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  const fs::path game = DlcFolderOf(user, ids[0].c_str()) / "game";
  MakeOldGame(game);
  CHECK(RemoveInstalledDlc(user, game, ids[0]).status == RemoveStatus::kUnsafeTarget);
  CHECK(OldGameIntact(game));
  CHECK(ReadFile(DlcFolderOf(user, ids[0].c_str()) / "addon.bin") == Pattern(7000, 10));
}

void TestUninstallOnlyVerifiedPackages() {
  Section("only packages whose header names this title, type and content ID are removed");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);

  const std::string bare = HexOf(IdFrom(0xB0));
  WriteFile(DlcFolderOf(user, bare.c_str()) / "x.bin", Pattern(50, 2));
  const std::string copied = HexOf(IdFrom(0xC0));
  WriteFile(DlcFolderOf(user, copied.c_str()) / "y.bin", Pattern(50, 3));
  WriteFile(DlcHeaderOf(user, copied.c_str()), ReadFile(DlcHeaderOf(user, ids[1].c_str())));
  const std::vector<DlcInfo> listed = ListInstalledDlc(user);
  CHECK(listed.size() == 5);
  CHECK(std::count_if(listed.begin(), listed.end(), [](const DlcInfo& info) { return info.verified; }) == 3);
  CHECK(RemoveInstalledDlc(user, game, bare).status == RemoveStatus::kUnverified);
  CHECK(RemoveInstalledDlc(user, game, copied).status == RemoveStatus::kUnverified);
  CHECK(ReadFile(DlcFolderOf(user, bare.c_str()) / "x.bin") == Pattern(50, 2));
  CHECK(ReadFile(DlcFolderOf(user, copied.c_str()) / "y.bin") == Pattern(50, 3));

  const std::string third = HexOf(IdFrom(0xD0));
  WriteFile(scratch.path() / "third.bin", BuildPackage(AddOnItems(20), AddOn(IdFrom(0xD0), u"Third Party Pack")));
  CHECK(InstallDlcFromPackage(scratch.path() / "third.bin", user).ok());
  const std::vector<DlcInfo> installed = ListInstalledDlc(user);
  const auto found = std::find_if(installed.begin(), installed.end(),
                                  [&](const DlcInfo& info) { return info.content_id == third; });
  CHECK(found != installed.end());
  if (found != installed.end()) {
    CHECK(found->verified);
    CHECK(found->display_name == "Third Party Pack");
  }
  CHECK(RemoveInstalledDlc(user, game, third).ok());
  CHECK(ListInstalledDlc(user).size() == 5);
}

void TestUninstallThenReinstall() {
  Section("a removed package installs again, and reinstalling the game keeps the DLC that remains");
  Scratch scratch(g_scratch_base);
  const fs::path user = scratch.path() / "Documents";
  const fs::path game = scratch.path() / "game";
  MakeOldGame(game);
  const std::array<std::string, 3> ids = InstallPackages(scratch.path(), user);
  CHECK(RemoveInstalledDlc(user, game, ids[0]).ok());
  CHECK(!fs::exists(DlcFolderOf(user, ids[0].c_str())));

  CHECK(InstallDlcFromPackage(scratch.path() / "package0.bin", user).ok());
  const std::vector<DlcInfo> listed = ListInstalledDlc(user);
  CHECK(listed.size() == 3);
  const auto back = std::find_if(listed.begin(), listed.end(),
                                 [&](const DlcInfo& info) { return info.content_id == ids[0]; });
  CHECK(back != listed.end());
  if (back != listed.end()) {
    CHECK(back->verified);
    CHECK(back->display_name == kPackageNames[0]);
  }
  CHECK(ReadFile(DlcFolderOf(user, ids[0].c_str()) / "addon.bin") == Pattern(7000, 10));

  WriteFile(scratch.path() / "game.bin", BuildPackage(ValidGamePackageItems()));
  CHECK(InstallFromPackage(scratch.path() / "game.bin", game).ok());
  CHECK(CheckGameFiles(game).state == GameFiles::kReady);
  CHECK(ListInstalledDlc(user).size() == 3);
  CHECK(NoDlcLeftovers(user));
}

void TestRealPackage(const fs::path& package, const fs::path& reference) {
  Section("real package: one extraction through the installer");
  Scratch scratch(g_scratch_base);
  const fs::path game = scratch.path() / "game";
  stfs::Package pkg;
  CHECK(pkg.Open(package).ok());
  std::printf("  title id %08X\n", pkg.title_id());
  CHECK(pkg.title_id() == kTitleId);
  CHECK(pkg.title_id() == 0x584109D2);
  CHECK(pkg.LoadFiles().ok());
  std::printf("  %zu files, %llu bytes in the file table\n", pkg.file_count(),
              static_cast<unsigned long long>(pkg.total_bytes()));

  uint64_t progress_calls = 0;
  int phases = 0;
  InstallHooks hooks;
  hooks.phase = [&](const char* text) {
    ++phases;
    std::printf("  phase: %s\n", text);
  };
  hooks.progress = [&](uint64_t, uint64_t) {
    ++progress_calls;
    return true;
  };
  const auto started = std::chrono::steady_clock::now();
  const InstallResult result = InstallFromPackage(package, game, hooks);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("  install: %s in %.1f s, %zu files, %llu bytes, %llu progress calls\n",
              DescribeInstall(result.status).title, seconds, result.files,
              static_cast<unsigned long long>(result.bytes), static_cast<unsigned long long>(progress_calls));
  CHECK(result.ok());
  const GameCheck check = CheckGameFiles(game);
  CHECK(check.state == GameFiles::kReady);
  std::printf("  default.xex CRC32 %08X\n", check.xex_crc32);
  std::printf("  %s CRC32 %08X\n", kModuleXexName, check.module_xex_crc32);
  CHECK(check.xex_crc32 == kXexCrc32);
  CHECK(check.module_xex_crc32 == kModuleXexCrc32);
  CHECK(fs::is_regular_file(game / "default.xex"));
  CHECK(fs::is_regular_file(game / kModuleXexName));
  CHECK(fs::is_regular_file(game / "ArcadeInfo.xml"));
  CHECK(fs::is_directory(game / "Data"));
  CHECK(fs::is_regular_file(game / "Data" / "TMNT.bf"));
  CHECK(phases == 4);
  CHECK(NoInstallLeftovers(scratch.path()));

  size_t files = 0, dirs = 0;
  for (const auto& e : fs::recursive_directory_iterator(game)) {
    e.is_directory() ? ++dirs : ++files;
  }
  std::printf("  extracted %zu files, %zu directories\n", files, dirs);
  if (reference.empty()) {
    return;
  }
  size_t compared = 0, mismatched = 0, ref_files = 0;
  for (const auto& e : fs::recursive_directory_iterator(reference)) {
    if (e.is_directory()) {
      continue;
    }
    ++ref_files;
    const fs::path mine = game / fs::relative(e.path(), reference);
    if (!fs::exists(mine) || fs::file_size(mine) != e.file_size() || !SameFile(mine, e.path())) {
      ++mismatched;
    }
    ++compared;
  }
  std::printf("  compared %zu files with the reference extraction, %zu differ\n", compared, mismatched);
  CHECK(mismatched == 0);
  CHECK(ref_files == files);
}

}  // namespace

int main(int argc, char** argv) {
  fs::path package, reference;
  g_scratch_base = fs::temp_directory_path() / "tmnt_installer_tests";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--scratch") {
      g_scratch_base = argv[i + 1];
    } else if (flag == "--package") {
      package = argv[i + 1];
    } else if (flag == "--reference") {
      reference = argv[i + 1];
    }
  }
  fs::remove_all(g_scratch_base);
  fs::create_directories(g_scratch_base);

  TestHeader();
  TestNotPackage();
  TestWrongTitle();
  TestUnsafeNames();
  TestUnsafeStructure();
  TestUnsupportedLayouts();
  TestExtractionStaysInside();
  TestBlockBoundary();
  TestTruncatedAndPartial();
  TestInvalidXex();
  TestValidInstallAndReinstall();
  TestForeignFolder();
  TestCaseVariantGameNames();
  TestLauncherDecision();
  TestOnWorkerThread();
  TestDlcMetadata();
  TestDlcRejections();
  TestDlcInstallSideBySide();
  TestDlcFailuresKeepInstalled();
  TestDlcUnsafeEntries();
  TestDlcWorkFolderCleanup();
  TestDlcInstallRefusesLinks();
  TestDlcListing();
  TestUninstallEachPackage();
  TestUninstallKeepsUserData();
  TestUninstallRejectsBadTargets();
  TestFailedUninstallIsNotSuccess();
  TestUninstallRefusesLinks();
  TestUninstallRefusesGameFolderInside();
  TestUninstallOnlyVerifiedPackages();
  TestUninstallThenReinstall();
  if (!package.empty()) {
    TestRealPackage(package, reference);
  }

  std::error_code ec;
  fs::remove_all(g_scratch_base, ec);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
