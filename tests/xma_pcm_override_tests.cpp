// Patch 0087: the generic XMA PCM override state machine in the patched SDK's src/audio/xma_pcm_override.h.
// tools/test.ps1 builds it against the patch stack applied to the pinned SDK. Run: xma_pcm_override_tests

#include <cstdio>
#include <vector>

#include "xma_pcm_override.h"

// The patched xma_context.cpp defines this the same way.
extern "C" void rex_audio_set_xma_pcm_provider(const RexXmaPcmProvider* provider) {
  rex::audio::pcm_override::SetProvider(provider);
}

namespace {

using namespace rex::audio::pcm_override;

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

struct Fake {
  int opens = 0;
  int renders = 0;
  int closes = 0;
  int32_t answer = kNoStream;
  int32_t render_result = rex::audio::pcm_override::kRenderReplaced;
  int32_t last_closed = kNoStream;
  uint32_t last_rate = 0;
  uint32_t last_channels = 0;
};

Fake g_fake;

RexXmaPcmProvider FakeProvider() {
  RexXmaPcmProvider provider{};
  provider.user = &g_fake;
  provider.open = [](void* user, uint32_t, const uint8_t*, uint32_t, uint32_t rate, uint32_t channels) {
    auto* fake = static_cast<Fake*>(user);
    ++fake->opens;
    fake->last_rate = rate;
    fake->last_channels = channels;
    return fake->answer;
  };
  provider.render = [](void* user, int32_t, int16_t* frames, uint32_t count) {
    auto* fake = static_cast<Fake*>(user);
    ++fake->renders;
    for (uint32_t i = 0; i < count * 2; ++i) {
      frames[i] = int16_t(0x1234);
    }
    return fake->render_result;
  };
  provider.close = [](void* user, int32_t stream) {
    auto* fake = static_cast<Fake*>(user);
    ++fake->closes;
    fake->last_closed = stream;
  };
  return provider;
}

const uint8_t kData[16] = {};
int16_t g_scratch[512 * 2];

void TestNoProvider() {
  std::printf("no provider: decoded audio is untouched\n");
  rex_audio_set_xma_pcm_provider(nullptr);
  ContextState state;
  OnBuffer(state, 0, 0x1000, kData, 4096, 44100, 2);
  CHECK(state.stream == kNoStream);
  std::vector<uint8_t> frame(2048, 0xAB);
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch));
  CHECK(frame[0] == 0xAB && frame[2047] == 0xAB);
}

void TestReplacement() {
  std::printf("known stream: open once per buffer, fill only the given blocks, big-endian\n");
  g_fake = {};
  const RexXmaPcmProvider provider = FakeProvider();
  rex_audio_set_xma_pcm_provider(&provider);
  ContextState state;

  g_fake.answer = kNoStream;
  OnBuffer(state, 3, 0x1000, kData, 4096, 44100, 2);
  CHECK(g_fake.opens == 1 && state.stream == kNoStream);
  std::vector<uint8_t> frame(2048, 0xAB);
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch) && g_fake.renders == 0);

  g_fake.answer = 2;
  OnBuffer(state, 3, 0x2000, kData, 8192, 48000, 2);
  CHECK(g_fake.opens == 2 && state.stream == 2 && g_fake.last_rate == 48000 && g_fake.last_channels == 2);
  OnBuffer(state, 3, 0x2000, kData, 8192, 48000, 2);
  CHECK(g_fake.opens == 2);

  CHECK(Fill(state, frame.data(), 2, 6, g_scratch));
  CHECK(g_fake.renders == 1);
  CHECK(frame[2 * kBlockBytes - 1] == 0xAB && frame[6 * kBlockBytes] == 0xAB);
  CHECK(frame[2 * kBlockBytes] == 0x12 && frame[2 * kBlockBytes + 1] == 0x34 && frame[6 * kBlockBytes - 1] == 0x34);
  CHECK(!Fill(state, frame.data(), 5, 5, g_scratch) && g_fake.renders == 1);

  g_fake.answer = kNoStream;
  OnBuffer(state, 3, 0x3000, kData, 4096, 48000, 2);
  CHECK(state.stream == 2 && g_fake.closes == 0);

  OnClear(state);
  g_fake.answer = 2;
  OnBuffer(state, 3, 0x2000, kData, 8192, 48000, 2);
  CHECK(state.stream == 2 && g_fake.closes == 0 && g_fake.opens == 4);

  OnClear(state);
  g_fake.answer = kNoStream;
  OnBuffer(state, 3, 0x2000, kData, 8192, 48000, 2);
  CHECK(state.stream == kNoStream && g_fake.closes == 1 && g_fake.last_closed == 2);

  g_fake.answer = 4;
  OnBuffer(state, 3, 0x4000, kData, 4096, 44100, 2);
  CHECK(state.stream == 4);
  g_fake.answer = 5;
  OnBuffer(state, 3, 0x5000, kData, 4096, 44100, 2);
  CHECK(state.stream == 5 && g_fake.closes == 2 && g_fake.last_closed == 4);
}

void TestPassThroughKeepsTheStream() {
  std::printf("pass-through keeps the decoded samples and the stream, then replacement resumes\n");
  g_fake = {};
  const RexXmaPcmProvider provider = FakeProvider();
  rex_audio_set_xma_pcm_provider(&provider);
  ContextState state;
  g_fake.answer = 2;
  OnBuffer(state, 0, 0x1000, kData, 4096, 44100, 2);
  std::vector<uint8_t> frame(2048, 0xAB);
  g_fake.render_result = kRenderPassThrough;
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch));
  CHECK(state.stream == 2 && g_fake.closes == 0 && g_fake.renders == 1);
  CHECK(frame[0] == 0xAB && frame[2047] == 0xAB);
  g_fake.render_result = kRenderReplaced;
  CHECK(Fill(state, frame.data(), 0, 8, g_scratch) && state.stream == 2 && g_fake.renders == 2);
  CHECK(frame[0] == 0x12 && frame[1] == 0x34);
  OnRelease(state);
}

void TestFailureAndRelease() {
  std::printf("provider failure falls back for the rest of the stream; release closes\n");
  g_fake = {};
  const RexXmaPcmProvider provider = FakeProvider();
  rex_audio_set_xma_pcm_provider(&provider);
  ContextState state;
  g_fake.answer = 1;
  OnBuffer(state, 0, 0x1000, kData, 4096, 44100, 2);
  g_fake.render_result = kRenderFailed;
  std::vector<uint8_t> frame(2048, 0xAB);
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch));
  CHECK(state.stream == kNoStream && g_fake.closes == 1);
  g_fake.render_result = kRenderReplaced;
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch) && g_fake.renders == 1);
  OnBuffer(state, 0, 0x1000, kData, 4096, 44100, 2);
  CHECK(g_fake.opens == 1);

  g_fake.answer = 7;
  OnBuffer(state, 0, 0x6000, kData, 4096, 44100, 2);
  CHECK(state.stream == 7);
  OnRelease(state);
  CHECK(state.stream == kNoStream && state.reset && state.buffer_address == 0 && g_fake.last_closed == 7);

  OnBuffer(state, 0, 0x6000, kData, 4096, 44100, 2);
  CHECK(state.stream == 7);
  rex_audio_set_xma_pcm_provider(nullptr);
  CHECK(!Fill(state, frame.data(), 0, 8, g_scratch) && state.stream == kNoStream);
}

}  // namespace

int main() {
  TestNoProvider();
  TestReplacement();
  TestPassThroughKeepsTheStream();
  TestFailureAndRelease();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
