// Shredder lighting flicker fix (patches/rexglue-sdk/0090-shredder-tint-fetch-fix.windows.patch). tools/test.ps1 extracts
// the header from the patch, so the code under test is the shipped code; the translator and command processor wiring
// needs a GPU, so the patch text is inspected.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/graphics/d3d12/shredder_tint_fetch_fix.h>

namespace {

using namespace rex::graphics::d3d12;

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

constexpr uint64_t kConstantTintShader = 0x9D8D9425A3202246ull;
constexpr uint64_t kOtherVertexShader = 0x3AD3B3C0A39B3540ull;
constexpr uint64_t kAa838Shader = 0xAA83877EDB2163EEull;
constexpr const char* kPatch = "patches/rexglue-sdk/0090-shredder-tint-fetch-fix.windows.patch";

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::string(TMNT_ROOT_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

void TestTranslationMatch() {
  CHECK(ShredderTintFixMatchesFetch(kShredderTintVertexShader, 94, false, 1));
  CHECK(!ShredderTintFixMatchesFetch(kConstantTintShader, 94, false, 1));
  CHECK(!ShredderTintFixMatchesFetch(kOtherVertexShader, 94, false, 1));
  CHECK(!ShredderTintFixMatchesFetch(0, 94, false, 1));
  CHECK(!ShredderTintFixMatchesFetch(kShredderTintVertexShader, 95, false, 6));
  CHECK(!ShredderTintFixMatchesFetch(kShredderTintVertexShader, 93, false, 1));
  CHECK(!ShredderTintFixMatchesFetch(kShredderTintVertexShader, 94, true, 1));
  CHECK(!ShredderTintFixMatchesFetch(kShredderTintVertexShader, 94, false, 0));
}

void TestDrawMatch() {
  CHECK(ShredderTintFixMatchesDraw(kShredderTintVertexShader, kShredderTintPixelShader, true, 1));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, kShredderTintPixelShader, true, 92));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, kShredderTintPixelShader, true, 2));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, kShredderTintPixelShader, true, 0));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, kShredderTintPixelShader, false, 1));
  CHECK(!ShredderTintFixMatchesDraw(kConstantTintShader, kShredderTintPixelShader, true, 1));
  CHECK(!ShredderTintFixMatchesDraw(kOtherVertexShader, kShredderTintPixelShader, true, 1));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, kAa838Shader, true, 1));
  CHECK(!ShredderTintFixMatchesDraw(kShredderTintVertexShader, 0, true, 1));
}

void TestEffectiveAddress() {
  const uint32_t base = 0x1DF90000;
  for (uint32_t index : {0u, 1u, 2753u}) {
    CHECK(ShredderTintFetchAddress(base, index, 1, true) == base);
    CHECK(ShredderTintFetchAddress(base, index, 1, false) == base + index * 4);
  }
  CHECK(ShredderTintFetchAddress(0x19293000, 7, 6, false) == 0x19293000 + 7 * 24);
}

void TestSlotAndPatchWiring() {
  CHECK(kShredderTintFixBool == 121);
  const std::string patch = ReadFile(kPatch);
  CHECK(!patch.empty());
  CHECK(Has(patch, "+      if (d3d12::ShredderTintFixMatchesFetch(current_shader().ucode_data_hash(),"));
  CHECK(Has(patch, "+    shredder_tint_fix_pending_ = ShredderTintFixMatchesDraw("));
  CHECK(Has(patch, "+  shredder_tint_fix_pending_ = false;"));
  CHECK(Has(patch, "+      word = shredder_tint_fix_bool_state_ ? (word | bit) : (word & ~bit);"));
  // The address of the fetch is replaced by the base only: no index or stride is touched.
  CHECK(Has(patch, "+  a_.OpAnd(address_dest, fetch_constant_src.SelectFromSwizzled(0), dxbc::Src::LU(~uint32_t(3)));"));
  // Production: no diagnostics, no hardcoded allocation address, and the AA838 fix is neither touched nor combined.
  CHECK(!Has(patch, "DEBUG_SAULO") && !Has(patch, "REXGPU_WARN") && !Has(patch, "REXGPU_INFO"));
  CHECK(!Has(patch, "19227000") && !Has(patch, "1DF90000") && !Has(patch, "0x1DF9"));
  std::istringstream lines(patch);
  size_t removed = 0;
  bool aa838_touched = false;
  for (std::string line; std::getline(lines, line);) {
    const bool added = line.size() > 1 && line[0] == '+' && line.compare(0, 3, "+++") != 0;
    const bool deleted = line.size() > 1 && line[0] == '-' && line.compare(0, 3, "---") != 0;
    removed += deleted;
    if (added || deleted) {
      aa838_touched |= Has(line, "kAa838") || Has(line, "aa838_stage_a") || Has(line, "ApplyAa838StageAFix") ||
                       Has(line, "AA83877EDB2163EE");
    }
  }
  CHECK(!aa838_touched);
  CHECK(removed == 0);
  const std::string aa838 = ReadFile("patches/rexglue-sdk/0080-tmnt-aa838-stage-a-fix.windows.patch");
  CHECK(Has(aa838, "constexpr uint32_t kAa838StageAByte1ZeroBool = 120;"));
  const std::string manifest = ReadFile("patches/rexglue-sdk/patches.txt");
  CHECK(Has(manifest, "0090-shredder-tint-fetch-fix.windows.patch"));
}

}  // namespace

int main() {
  TestTranslationMatch();
  TestDrawMatch();
  TestEffectiveAddress();
  TestSlotAndPatchWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
