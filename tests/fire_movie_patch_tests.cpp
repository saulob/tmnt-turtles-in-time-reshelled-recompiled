#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fire_movie_patch.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(expr)) {                                                  \
      ++g_failures;                                                 \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    }                                                               \
  } while (0)

using namespace tmnt;

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(TMNT_SOURCE_DIR) + "/src/" + name, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::vector<uint8_t> MovieWithExpectedBytes() {
  std::vector<uint8_t> movie(kOnlineMenuMovieBytes, 0);
  for (const MovieBytePatch& patch : kOnlineMenuPatches) movie[patch.offset] = patch.before;
  return movie;
}

void TestPatchesAreAppliedTogetherOrNotAtAll() {
  std::vector<uint8_t> movie = MovieWithExpectedBytes();
  const size_t count = sizeof kOnlineMenuPatches / sizeof kOnlineMenuPatches[0];
  CHECK(ApplyMovieBytePatches(movie.data(), movie.size(), kOnlineMenuPatches, count));
  for (const MovieBytePatch& patch : kOnlineMenuPatches) CHECK(movie[patch.offset] == patch.after);
  CHECK(!ApplyMovieBytePatches(movie.data(), movie.size(), kOnlineMenuPatches, count));  // already patched

  std::vector<uint8_t> altered = MovieWithExpectedBytes();
  altered[kOnlineMenuPatches[count - 1].offset] ^= 0xFF;
  const std::vector<uint8_t> before = altered;
  CHECK(!ApplyMovieBytePatches(altered.data(), altered.size(), kOnlineMenuPatches, count));
  CHECK(altered == before);  // the first patches were not applied either
  CHECK(!ApplyMovieBytePatches(altered.data(), 10, kOnlineMenuPatches, count));
  CHECK(!ApplyMovieBytePatches(nullptr, 0, kOnlineMenuPatches, count));
}

void TestOnlyTheKnownMovieIsPatched() {
  std::vector<uint8_t> movie = MovieWithExpectedBytes();
  CHECK(!PatchOnlineMenuMovie(movie.data(), movie.size()));      // right size, different content
  CHECK(!PatchOnlineMenuMovie(movie.data(), movie.size() - 1));  // another file
  CHECK(!PatchOnlineMenuMovie(nullptr, kOnlineMenuMovieBytes));
  CHECK(movie == MovieWithExpectedBytes());
}

void TestPatchTakesTheMoviesOwnThreeRowPath() {
  CHECK(kOnlineMenuMovieBytes == 115401);
  CHECK(kOnlineMenuPatches[0].offset == 6054);  // the else branch's ActionJump offset, not the ActionIf at 6046
  CHECK(kOnlineMenuPatches[0].before == 30 && kOnlineMenuPatches[0].after == 0);
  CHECK(kOnlineMenuPatches[1].before == 4 && kOnlineMenuPatches[1].after == 3);
  CHECK(kOnlineMenuPatches[2].before == 4 && kOnlineMenuPatches[2].after == 3);
  const std::string hook = ReadSource("fire_menu_hook.cpp");
  CHECK(Has(hook, "REX_HOOK_RAW(sub_824EC4C8)") && Has(hook, "size != tmnt::kOnlineMenuMovieBytes"));
  CHECK(hook.find("__imp__sub_824EC4C8(ctx, base);") < hook.find("PatchOnlineMenuMovie"));
}

void TestMenuTextIsRenamedAtTheFirePackage() {
  const std::string dll = ReadSource("live_hooks_tmnt_r.cpp");
  CHECK(Has(dll, "constexpr char kXboxLiveLabel[] = \"XBOX LIVE\";"));
  CHECK(Has(dll, "constexpr char kOnlineLabel[] = \"ONLINE\";"));
  CHECK(Has(dll, "if (id == kMenuTextPackage && strings) RenameXboxLiveLabel(base, strings);"));
  CHECK(!Has(dll, "XBOX LIVE PARTY"));
}

}  // namespace

int main() {
  TestPatchesAreAppliedTogetherOrNotAtAll();
  TestOnlyTheKnownMovieIsPatched();
  TestPatchTakesTheMoviesOwnThreeRowPath();
  TestMenuTextIsRenamedAtTheFirePackage();
  std::printf("fire_movie_patch_tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
