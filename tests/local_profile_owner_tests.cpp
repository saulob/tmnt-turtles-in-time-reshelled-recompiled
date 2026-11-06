// The controller that presses START owns the one local profile, whichever slot it connected in, so the title's
// "You are not signed in" warning cannot appear and that controller saves to the existing profile. Runs without a game.
// Run: tmnt_local_profile_owner_tests

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "live_scope.h"
#include "player_match.h"

using namespace tmnt;
using namespace tmnt::live;

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

constexpr uint32_t kUsers = 4;
constexpr uint64_t kLocalXuid = 0xE000000000000001;

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string ReadSource(const std::string& name) { return ReadFile(std::filesystem::path(TMNT_SRC_DIR) / name); }

bool Has(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

std::string Between(const std::string& text, std::string_view from, std::string_view to) {
  const size_t start = text.find(from);
  if (start == std::string::npos) return {};
  const size_t end = text.find(to, start + from.size());
  return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// What the runtime answers: only user 0 exists.
uint32_t RuntimeSigninState(uint32_t user) { return user == 0 ? 1 : 0; }
uint64_t RuntimeXuid(uint32_t user) { return user == 0 ? kLocalXuid : 0; }

struct Console {
  uint32_t owner = 0;
  uint32_t online_user = 0;
  bool live = false;

  uint32_t SigninState(uint32_t user) const {
    return LiveProfileUser(user, online_user, live) ? kSignedInToLive : RuntimeSigninState(LocalProfileSlot(user, owner));
  }
  uint64_t OfflineId(uint32_t user) const { return RuntimeXuid(LocalProfileSlot(user, owner)); }
  bool CanSave(uint32_t user) const { return LocalProfileSlot(user, owner) == 0; }
  bool WarnsOnStart(uint32_t pad) const { return SigninState(pad) == 0; }
  void PressStart(uint32_t pad) { owner = online_user = pad; }
};

void TestTheWarningNeedsAnEmptySlot() {
  std::printf("a controller that is not user 0 reads as not signed in until it owns the profile\n");
  const Console untouched;
  CHECK(!untouched.WarnsOnStart(0));
  for (uint32_t pad = 1; pad < kUsers; ++pad) {
    CHECK(untouched.WarnsOnStart(pad));
    CHECK(untouched.OfflineId(pad) == 0);
    CHECK(!untouched.CanSave(pad));
  }
}

void TestTheOwnerIsTheLocalProfile() {
  std::printf("the controller that presses START is signed in, has the saved identity and can save\n");
  for (const bool live : {false, true}) {
    for (uint32_t pad = 0; pad < kUsers; ++pad) {
      Console console;
      console.live = live;
      console.PressStart(pad);
      CHECK(!console.WarnsOnStart(pad));
      CHECK(console.SigninState(pad) == (live ? kSignedInToLive : 1));
      CHECK(console.OfflineId(pad) == kLocalXuid);
      CHECK(console.CanSave(pad));
      uint32_t signed_in = 0;
      for (uint32_t user = 0; user < kUsers; ++user) signed_in += console.SigninState(user) != 0;
      CHECK(signed_in == 1);
    }
  }
}

void TestTheSlotMapIsASwap() {
  std::printf("the slot map is a swap, the identity for user 0, and leaves any-user alone\n");
  for (uint32_t user = 0; user < kUsers; ++user) CHECK(LocalProfileSlot(user, 0) == user);
  for (uint32_t owner = 0; owner < kUsers; ++owner) {
    CHECK(LocalProfileSlot(owner, owner) == 0);
    CHECK(LocalProfileSlot(0, owner) == owner);
    CHECK(LocalProfileSlot(kAnyUser, owner) == kAnyUser);
    for (uint32_t user = 0; user < kUsers; ++user) {
      CHECK(LocalProfileSlot(LocalProfileSlot(user, owner), owner) == user);
    }
  }
}

void TestOnlyTheWarningSitesClaim() {
  std::printf("only Press START and Single Player claim the profile\n");
  constexpr uint32_t kPressStart = 0;
  CHECK(ClaimsLocalProfile(kFireEventProceedSignal, kPressStart));
  CHECK(ClaimsLocalProfile(kFireEventProceedSignal, kProceedSourceSinglePlayer));
  CHECK(ClaimsLocalProfile(kFireEventProceedSignal, kProceedSourceLast + 1));
  for (uint32_t source = 1; source <= kProceedSourceLast; ++source) {
    if (source != kProceedSourceSinglePlayer) CHECK(!ClaimsLocalProfile(kFireEventProceedSignal, source));
  }
  for (uint32_t event = 0; event < 300; ++event) {
    if (event != kFireEventProceedSignal) CHECK(!ClaimsLocalProfile(event, kPressStart));
  }
}

void TestHooksAnswerFromTheProfileSlot() {
  std::printf("every local answer goes through the slot, and so do the save reads and writes\n");
  const std::string hook = ReadSource("player_match_hook.cpp");
  const std::string state = Between(hook, "void SigninState(", "void CheckPrivilege(");
  CHECK(Has(state, "tmnt::kSignedInToLive"));
  CHECK(state.find("ctx.r3.u64 = LocalProfileSlot(user);") < state.find("original(ctx, base);"));
  CHECK(Has(state, "ctx.r3.u64 = LocalProfileSlot(user);"));
  for (const auto& [from, to] : {std::pair{"void CheckPrivilege(", "void SigninInfo("},
                                 std::pair{"void SigninInfo(", "void GetXuid("},
                                 std::pair{"void GetXuid(", "// Also the name"},
                                 std::pair{"void GetName(", "void ReadTitleProfile("}}) {
    const std::string body = Between(hook, from, to);
    CHECK(Has(body, "ctx.r3.u64 = LocalProfileSlot(user);"));
    CHECK(body.find("ctx.r3.u64 = LocalProfileSlot(user);") < body.find("original(ctx, base);"));
  }
  const std::string read = Between(hook, "void ReadTitleProfile(", "void WriteTitleProfile(");
  CHECK(read.find("ctx.r4.u64 = LocalProfileSlot(ctx.r4.u32);") < read.find("original(ctx, base);"));
  CHECK(Has(read, "ctx.r4.u64 = LocalProfileSlot(ctx.r4.u32);"));
  const std::string write = Between(hook, "void WriteTitleProfile(", "void TitleXnAddr(");
  CHECK(write.find("ctx.r3.u64 = LocalProfileSlot(ctx.r3.u32);") < write.find("original(ctx, base);"));
  CHECK(Has(write, "ctx.r3.u64 = LocalProfileSlot(ctx.r3.u32);"));
  for (const char* line : {"TMNT_LIVE_HOOK(826EDD50, SigninState)", "TMNT_LIVE_HOOK(826EDD60, CheckPrivilege)",
                           "TMNT_LIVE_HOOK(826EE0C8, SigninInfo)", "TMNT_LIVE_HOOK(826EE078, GetXuid)",
                           "TMNT_LIVE_HOOK(826EDD48, GetName)", "TMNT_LIVE_HOOK(82A28FD8, ReadTitleProfile)",
                           "TMNT_LIVE_HOOK(82A29040, WriteTitleProfile)"}) {
    CHECK(Has(hook, line));
  }
}

void TestTheClaimComesFirst() {
  std::printf("the claim happens before the event's own sign-in check\n");
  const std::string module = ReadSource("live_hooks_tmnt_r.cpp");
  const std::string events = Between(module, "REX_HOOK_RAW(sub_875BA998)", "REX_EXTERN(__imp__sub_875CCEC8)");
  CHECK(Has(events, "ClaimsLocalProfile(LoadBE32(base, var), LoadBE32(base, var + kFireEventSourceOffset))"));
  CHECK(Has(events, "ClaimLocalProfile(int32_t(LoadBE32(base, var + kFireEventControllerOffset)))"));
  CHECK(events.find("ClaimLocalProfile(") < events.find("__imp__sub_875BA998(ctx, base);"));
  const std::string claim = Between(module, "void ClaimLocalProfile(", "void LogLocalProfileAccepted(");
  CHECK(Has(claim, "IsLocalController(controller)"));
  CHECK(Has(claim, "\"tmnt_local_profile_user\""));
  CHECK(Has(claim, "\"tmnt_online_user\""));
}

void TestNoDependencyOnTheOnlineService() {
  std::printf("the local profile never reads the online service, and no dialog is suppressed\n");
  const std::string hook = ReadSource("player_match_hook.cpp");
  const std::string slot = Between(hook, "uint32_t LocalProfileSlot(uint32_t user) {", "struct LiveProfile {");
  CHECK(!slot.empty());
  for (const char* online : {"CurrentLive", "OnlineRunning", "Online()", "signed_in", "friend_code"}) {
    CHECK(!Has(slot, online));
  }
  const std::string module = ReadSource("live_hooks_tmnt_r.cpp");
  const std::string claim = Between(module, "void ClaimLocalProfile(", "void LogLocalProfileAccepted(");
  for (const char* online : {"OnlineRunning", "Online()", "LiveScope"}) CHECK(!Has(claim, online));
  CHECK(!Has(module, "SendWarningOverlay"));
  CHECK(!Has(hook, "SendWarningOverlay"));
  CHECK(Has(module, "LocalPlayerJoin joining;"));
  for (const char* file : {"online_runtime.cpp", "online_bridge.cpp", "online/online_service.cpp"}) {
    CHECK(!Has(ReadSource(file), "tmnt_local_profile_user"));
  }
}

class Image {
 public:
  Image(const std::filesystem::path& path, uint32_t image_base) : bytes_(ReadFile(path)), base_(image_base) {}
  bool empty() const { return bytes_.empty(); }
  uint32_t word(uint32_t at) const {
    const size_t offset = at - base_;
    if (offset + 4 > bytes_.size()) return 0;
    uint32_t value;
    std::memcpy(&value, bytes_.data() + offset, 4);
    return std::byteswap(value);
  }

 private:
  std::string bytes_;
  uint32_t base_;
};

void TestTheShippedGame() {
  const std::filesystem::path analysis = std::filesystem::path(TMNT_SRC_DIR) / ".." / "game" / "analysis";
  const Image module(analysis / "tmnt_r.image.bin", 0x87500000);
  const Image game(analysis / "default.image.bin", 0x82000000);
  if (module.empty() || game.empty()) {
    std::printf("shipped game: images not available, skipped\n");
    return;
  }
  std::printf("the proceed switch, the sign-in check and the save wrappers in the shipped game\n");
  CHECK(module.word(0x875BE520) == 0x397CFFFF);  // addi r11,r28,-1: source - 1
  CHECK(module.word(0x875BE524) == 0x2B0B0007);  // cmplwi cr6,r11,7: sources 1 to 8 have a case
  CHECK(module.word(0x875BE528) == 0x419906F4);  // bgt cr6,0x875BEC1C: Press START
  CHECK(kProceedSourceLast == 8);

  const uint32_t table = 0x820DE280 + 8 * kImportPlayerSignedIntoConsole;
  CHECK(game.word(table) == 0x822FD0C8);
  CHECK(game.word(0x822FD0D4) == 0x483F0C7D);  // bl sub_826EDD50: the check is the runtime's sign-in state
  constexpr uint32_t kMflrR12 = 0x7D8802A6;
  CHECK(game.word(0x82A28FD8) == kMflrR12);
  CHECK(game.word(0x82A29040) == kMflrR12);
}

}  // namespace

int main() {
  TestTheWarningNeedsAnEmptySlot();
  TestTheOwnerIsTheLocalProfile();
  TestTheSlotMapIsASwap();
  TestOnlyTheWarningSitesClaim();
  TestHooksAnswerFromTheProfileSlot();
  TestTheClaimComesFirst();
  TestNoDependencyOnTheOnlineService();
  TestTheShippedGame();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
