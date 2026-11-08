// AA838 Stage A classifier (patches/rexglue-sdk/0080-tmnt-aa838-stage-a-fix.windows.patch) and its DEBUG_SAULO
// telemetry (0088-debug-saulo-aa838-telemetry.windows.patch). CMake extracts both from the patches, so the code under
// test is the shipped code; the command processor and translator wiring needs a GPU, so the patch text is inspected.
// Run: tmnt_aa838_telemetry_tests

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <rex/graphics/d3d12/aa838_stage_fix.h>
#include <rex/graphics/d3d12/debug_saulo_aa838_telemetry.h>
#include <rex/graphics/flags.h>

#include "multisampling.h"

namespace {

using rex::graphics::d3d12::Aa838DrawRecord;
using rex::graphics::d3d12::Aa838StageAFix;
using Telemetry = rex::graphics::d3d12::DebugSauloAa838Telemetry;
using tmnt::HostMsaaSamples;

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

constexpr uint64_t kOtherShader = 0x47184C4711FA376Bull;

std::string ReadPatch(const char* name) {
  std::ifstream in(std::string(TMNT_ROOT_DIR) + "/patches/rexglue-sdk/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

// The lines a patch adds to one file.
std::string AddedLines(const std::string& patch, const char* path) {
  std::string header = std::string("+++ b/") + path + "\n";
  size_t start = patch.find(header);
  if (start == std::string::npos) {
    return {};
  }
  size_t end = patch.find("\ndiff --git ", start);
  std::istringstream lines(patch.substr(start + header.size(), end == std::string::npos ? std::string::npos
                                                                                        : end - start - header.size()));
  std::string added;
  for (std::string line; std::getline(lines, line);) {
    if (line.size() > 1 && line[0] == '+') {
      added += line.substr(1) + "\n";
    }
  }
  return added;
}

size_t Count(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

size_t CountStatements(const std::string& text, const std::string& keyword) {
  std::istringstream lines(text);
  size_t count = 0;
  for (std::string line; std::getline(lines, line);) {
    size_t first = line.find_first_not_of(' ');
    count += first != std::string::npos && line.compare(first, keyword.size(), keyword) == 0;
  }
  return count;
}

struct Capture {
  std::vector<std::string> lines;
  Telemetry telemetry{[this](const std::string& line) { lines.push_back(line); }};

  size_t CountContaining(const char* needle) const {
    size_t count = 0;
    for (const std::string& line : lines) {
      count += Has(line, needle);
    }
    return count;
  }
};

Aa838DrawRecord Drawn(uint64_t frame, bool stage_a) {
  Aa838DrawRecord record;
  record.frame = frame;
  record.ps_hash = kAa838InterleavedCopyPixelShader;
  record.ps_modification = 0x10;
  record.reached = Aa838DrawRecord::Reached::kDraw;
  record.stage_a_requested = stage_a;
  record.bool_bound = stage_a;
  record.surface_pitch = 1280;
  record.viewport[2] = 2560;
  record.viewport[3] = 1440;
  record.source_address = 0x1F360000;
  return record;
}

// Feeds a frame of AA838 draws the way the command processor does: the production classifier decides Stage A,
// telemetry gets the AA838 draws only.
void RunFrame(Aa838StageAFix& fix, Telemetry& telemetry, uint64_t frame, const std::vector<uint64_t>& shaders) {
  for (uint64_t shader : shaders) {
    bool stage_a = fix.OnDraw(shader);
    if (shader == kAa838InterleavedCopyPixelShader) {
      telemetry.OnDraw(Drawn(frame, stage_a));
    }
  }
  fix.OnFrameEnd();
  telemetry.OnFrameEnd(frame);
}

constexpr uint64_t A = kAa838InterleavedCopyPixelShader;

void TestClassifierNormalSequence() {
  std::printf("classifier: A, B, C in draw order\n");
  Aa838StageAFix fix;
  CHECK(fix.OnDraw(A));
  CHECK(!fix.OnDraw(A));
  CHECK(!fix.OnDraw(A));
}

void TestClassifierFrameReset() {
  std::printf("classifier: the frame end resets the ordinal\n");
  Aa838StageAFix fix;
  CHECK(fix.OnDraw(A));
  CHECK(!fix.OnDraw(A));
  fix.OnFrameEnd();
  CHECK(fix.OnDraw(A));
  CHECK(!fix.OnDraw(A));
  CHECK(!fix.OnDraw(A));
  fix.OnFrameEnd();
  fix.OnFrameEnd();
  CHECK(fix.OnDraw(A));
}

void TestClassifierIgnoresOtherShaders() {
  std::printf("classifier: other shaders neither match nor move the ordinal\n");
  Aa838StageAFix fix;
  CHECK(!fix.OnDraw(kOtherShader));
  CHECK(!fix.OnDraw(0));
  CHECK(!fix.OnDraw(kOtherShader));
  CHECK(fix.OnDraw(A));
  CHECK(!fix.OnDraw(kOtherShader));
  CHECK(!fix.OnDraw(A));
}

void TestClassifierExtraDraws() {
  std::printf("classifier: a 4th or a 2nd sequence of AA838 draws is never Stage A\n");
  Aa838StageAFix fix;
  int stage_a = 0;
  for (int i = 0; i < 6; ++i) {
    stage_a += fix.OnDraw(A);
  }
  CHECK(stage_a == 1);
}

void TestTelemetryNormalSequence() {
  std::printf("telemetry: a normal A/B/C frame logs its sequence and no anomaly\n");
  Capture capture;
  Aa838StageAFix fix;
  capture.telemetry.OnTranslation(0x10, false, true);
  RunFrame(fix, capture.telemetry, 100, {kOtherShader, A, kOtherShader, A, A, kOtherShader});
  CHECK(capture.CountContaining("anomaly") == 0);
  CHECK(capture.CountContaining("AA838 translate mod=0000000000000010 rov=0 fix_compiled=1") == 1);
  CHECK(capture.CountContaining("AA838 sequence frame=100 draws=3 reason=first") == 1);
  CHECK(capture.CountContaining("ordinal=0 stage=A") == 1);
  CHECK(capture.CountContaining("ordinal=1 stage=B") == 1);
  CHECK(capture.CountContaining("ordinal=2 stage=C") == 1);
  CHECK(capture.CountContaining("stage=A ps=AA83877EDB2163EE mod=0000000000000010 stage_a_bool=1 uploaded=1 "
                                "guest_b120=0 fix=1 reached=draw") == 1);
  CHECK(capture.CountContaining("stage=B ps=AA83877EDB2163EE mod=0000000000000010 stage_a_bool=0 uploaded=0") == 1);
  CHECK(capture.CountContaining("src=tf-1@1F360000") == 3);
}

void TestTelemetryFrameReset() {
  std::printf("telemetry: each frame starts at ordinal 0\n");
  Capture capture;
  Aa838StageAFix fix;
  for (uint64_t frame = 1; frame <= 3; ++frame) {
    RunFrame(fix, capture.telemetry, frame, {A, A, A});
  }
  CHECK(capture.CountContaining("anomaly") == 0);
  CHECK(capture.CountContaining("ordinal=0 stage=A") == 3);
  CHECK(capture.CountContaining("ordinal=3") == 0);
}

void TestTelemetryFourthDraw() {
  std::printf("telemetry: a 4th AA838 draw is reported\n");
  Capture capture;
  Aa838StageAFix fix;
  for (uint64_t frame = 1; frame <= Telemetry::kFirstFramesLogged; ++frame) {
    RunFrame(fix, capture.telemetry, frame, {A, A, A});
  }
  RunFrame(fix, capture.telemetry, 9, {A, A, A, A});
  CHECK(capture.CountContaining("anomaly frame=9 kind=extra_draw ordinal=3") == 1);
  CHECK(capture.CountContaining("anomaly frame=9 kind=draw_count draws=4 expected=3") == 1);
  CHECK(capture.CountContaining("sequence frame=9 draws=4 reason=anomaly") == 1);
  CHECK(capture.CountContaining("ordinal=3 stage=extra") == 1);
  CHECK(capture.CountContaining("kind=two_stage_a") == 0);
}

void TestTelemetryTwoSequences() {
  std::printf("telemetry: two A/B/C sequences in one frame\n");
  Capture capture;
  Aa838StageAFix fix;
  RunFrame(fix, capture.telemetry, 5, {A, A, A, kOtherShader, A, A, A});
  CHECK(capture.CountContaining("kind=extra_draw") == 3);
  CHECK(capture.CountContaining("kind=draw_count draws=6") == 1);
  CHECK(capture.CountContaining("stage_a_bool=1") == 1);
}

void TestTelemetryShortFrame() {
  std::printf("telemetry: fewer draws than expected, and a frame end in the middle of a sequence\n");
  Capture capture;
  Aa838StageAFix fix;
  RunFrame(fix, capture.telemetry, 1, {A});
  RunFrame(fix, capture.telemetry, 2, {A, A});
  RunFrame(fix, capture.telemetry, 3, {});
  CHECK(capture.CountContaining("kind=draw_count draws=1") == 1);
  CHECK(capture.CountContaining("kind=draw_count draws=2") == 1);
  CHECK(capture.CountContaining("frame=3") == 0);
}

void TestTelemetryStageAAnomalies() {
  std::printf("telemetry: two Stage A draws, classifier disagreement, bool propagation\n");
  Capture capture;
  Aa838DrawRecord second_a = Drawn(7, true);
  capture.telemetry.OnDraw(Drawn(7, true));
  capture.telemetry.OnDraw(second_a);
  capture.telemetry.OnFrameEnd(7);
  CHECK(capture.CountContaining("kind=two_stage_a ordinal=1") == 1);
  CHECK(capture.CountContaining("kind=classifier_mismatch ordinal=1 requested=1") == 1);

  Capture bools;
  Aa838DrawRecord not_uploaded = Drawn(8, true);
  not_uploaded.bool_bound = false;
  Aa838DrawRecord leaked = Drawn(8, false);
  leaked.bool_bound = true;
  leaked.guest_bool = true;
  bools.telemetry.OnDraw(not_uploaded);
  bools.telemetry.OnDraw(leaked);
  bools.telemetry.OnFrameEnd(8);
  CHECK(bools.CountContaining("kind=stage_a_bool_not_uploaded ordinal=0") == 1);
  CHECK(bools.CountContaining("kind=bool_set_outside_stage_a ordinal=1 guest_bool=1") == 1);
  CHECK(bools.CountContaining("stage_a_bool=1 uploaded=0") == 1);
  CHECK(bools.CountContaining("guest_b120=1") == 1);
}

void TestTelemetryDroppedAndUncompiled() {
  std::printf("telemetry: dropped draws and translations without the fix\n");
  Capture capture;
  Aa838DrawRecord pending = Drawn(3, true);
  pending.reached = Aa838DrawRecord::Reached::kRenderTargets;
  pending.bool_bound = false;
  capture.telemetry.OnDraw(pending);
  CHECK(capture.CountContaining("kind=draw_dropped ordinal=0 reached=render_targets") == 1);
  CHECK(capture.CountContaining("stage_a_bool_not_uploaded") == 0);

  capture.telemetry.OnTranslation(0x20, true, false);
  CHECK(capture.CountContaining("translate mod=0000000000000020 rov=1 fix_compiled=0") == 1);
  CHECK(capture.CountContaining("kind=fix_not_compiled mod=0000000000000020 rov=1") == 1);
  Aa838DrawRecord rov = Drawn(3, false);
  rov.ps_modification = 0x20;
  rov.rov = true;
  capture.telemetry.OnDraw(rov);
  CHECK(capture.CountContaining("kind=drawn_without_fix ordinal=1 mod=0000000000000020") == 1);
  Aa838DrawRecord unknown = Drawn(3, false);
  unknown.ps_modification = 0x30;
  capture.telemetry.OnDraw(unknown);
  capture.telemetry.OnFrameEnd(3);
  CHECK(capture.CountContaining("mod=0000000000000020 stage_a_bool=0 uploaded=0 guest_b120=0 fix=0") == 1);
  CHECK(capture.CountContaining("mod=0000000000000030 stage_a_bool=0 uploaded=0 guest_b120=0 fix=?") == 1);
}

void TestTelemetryMsaa() {
  std::printf("telemetry: host 2x and host 4x Compatibility are told apart\n");
  CHECK(HostMsaaSamples(2, true, true) == 2);
  CHECK(HostMsaaSamples(2, false, true) == 4);
  CHECK(HostMsaaSamples(1, false, true) == 1);
  Capture capture;
  Aa838StageAFix fix;
  for (uint32_t host : {2u, 4u}) {
    for (uint64_t frame = 0; frame < Telemetry::kFirstFramesLogged + 1; ++frame) {
      for (int i = 0; i < 3; ++i) {
        Aa838DrawRecord record = Drawn(frame, fix.OnDraw(A));
        record.guest_msaa_samples = 2;
        record.host_msaa_samples = HostMsaaSamples(2, host == 2, true);
        capture.telemetry.OnDraw(record);
      }
      fix.OnFrameEnd();
      capture.telemetry.OnFrameEnd(frame);
    }
  }
  CHECK(capture.CountContaining("msaa=guest2/host2") >= 3);
  CHECK(capture.CountContaining("msaa=guest2/host4") == 3);
  CHECK(capture.CountContaining("reason=new_state") == 1);
}

void TestTelemetryAlternatingState() {
  std::printf("telemetry: a state that alternates per frame is logged once per state\n");
  Capture capture;
  Aa838StageAFix fix;
  for (uint64_t frame = 0; frame < 200; ++frame) {
    for (int i = 0; i < 3; ++i) {
      Aa838DrawRecord record = Drawn(frame, fix.OnDraw(A));
      record.stencil_ref_mask = (frame & 1) ? 0x00020303 : 0x00FFFF00;
      record.source_address = (frame & 1) ? 0x1F360000 : 0x1F540000;
      capture.telemetry.OnDraw(record);
    }
    fix.OnFrameEnd();
    capture.telemetry.OnFrameEnd(frame);
  }
  CHECK(capture.CountContaining("reason=first") == Telemetry::kFirstFramesLogged);
  CHECK(capture.CountContaining("reason=new_state") == 0);
  CHECK(capture.CountContaining("anomaly") == 0);
  for (int i = 0; i < 3; ++i) {
    Aa838DrawRecord record = Drawn(200, fix.OnDraw(A));
    record.stencil_ref_mask = 0x00010101;
    capture.telemetry.OnDraw(record);
  }
  fix.OnFrameEnd();
  capture.telemetry.OnFrameEnd(200);
  CHECK(capture.CountContaining("sequence frame=200 draws=3 reason=new_state") == 1);
  CHECK(capture.CountContaining("stencil=00010101/") == 3);
}

void TestTelemetryCaps() {
  std::printf("telemetry: caps hold, and nothing is logged once every cap is reached\n");
  Capture capture;
  Aa838StageAFix fix;
  uint64_t frames = uint64_t(Telemetry::kPeriodicFrameInterval) * (Telemetry::kMaxSummaries + 2);
  for (uint64_t frame = 0; frame < frames; ++frame) {
    std::vector<uint64_t> shaders = {A, A, A, A};
    for (uint64_t shader : shaders) {
      Aa838DrawRecord record = Drawn(frame, fix.OnDraw(shader));
      record.viewport[0] = uint32_t(frame);
      capture.telemetry.OnDraw(record);
    }
    fix.OnFrameEnd();
    capture.telemetry.OnFrameEnd(frame);
  }
  CHECK(capture.CountContaining("kind=extra_draw") == Telemetry::kMaxAnomalyLinesPerKind);
  CHECK(capture.CountContaining("kind=draw_count") == Telemetry::kMaxAnomalyLinesPerKind);
  CHECK(capture.CountContaining(" anomaly ") <= Telemetry::kMaxAnomalyLines);
  CHECK(capture.CountContaining("reason=anomaly") == Telemetry::kMaxAnomalyFrames);
  CHECK(capture.CountContaining("reason=first") == Telemetry::kFirstFramesLogged);
  CHECK(capture.CountContaining("reason=new_state") == Telemetry::kMaxNewSignatureFrames);
  CHECK(capture.CountContaining("reason=periodic frames=") == Telemetry::kMaxSummaries);
  CHECK(capture.CountContaining("draws=4 reason=periodic") <= Telemetry::kMaxPeriodicFrames);
  CHECK(capture.lines.size() < 2000);

  size_t before = capture.lines.size();
  for (uint64_t frame = frames; frame < frames + 4 * Telemetry::kPeriodicFrameInterval; ++frame) {
    for (int i = 0; i < 4; ++i) {
      Aa838DrawRecord record = Drawn(frame, fix.OnDraw(A));
      record.viewport[0] = uint32_t(frame);
      capture.telemetry.OnDraw(record);
    }
    fix.OnFrameEnd();
    capture.telemetry.OnFrameEnd(frame);
  }
  CHECK(capture.lines.size() == before);
  capture.telemetry.Flush("exit");
  capture.telemetry.Flush("exit");
  CHECK(capture.lines.size() == before + 1);
  CHECK(Has(capture.lines.back(), "summary reason=exit"));
  CHECK(Has(capture.lines.back(), "draws_per_frame=[0:0 1:0 2:0 3:0 4:"));
}

void TestTelemetrySummary() {
  std::printf("telemetry: the periodic summary counts frames and Stage A uploads\n");
  Capture capture;
  Aa838StageAFix fix;
  for (uint64_t frame = 0; frame < Telemetry::kPeriodicFrameInterval; ++frame) {
    RunFrame(fix, capture.telemetry, frame, (frame % 2) ? std::vector<uint64_t>{A, A, A} : std::vector<uint64_t>{});
  }
  CHECK(capture.CountContaining("summary reason=periodic frames=1800 aa838_frames=900 draws=2700 "
                                "draws_per_frame=[0:900 1:0 2:0 3:900 4:0 5+:0] stage_a_requested=900 "
                                "stage_a_bound=900 dropped=0 anomalies=0") == 1);
}

// Mirrors D3D12CommandProcessor::SetupContext: the render target cache exists before the texture cache, and
// the device line needs both. TestDeviceLinePlacement ties the mirror to the patch.
struct FakeCommandProcessor {
  struct RenderTargetCache {
    bool host_2x = false;
  };
  struct TextureCache {
    uint32_t scale = 2;
  };
  explicit FakeCommandProcessor(Telemetry& telemetry) : telemetry(telemetry) {}

  Telemetry& telemetry;
  std::unique_ptr<RenderTargetCache> render_target_cache;
  std::unique_ptr<TextureCache> texture_cache;

  void LogDevice() {
    if (!render_target_cache || !texture_cache) {
      return;
    }
    rex::graphics::d3d12::Aa838DeviceInfo info;
    info.vendor_id = 0x10DE;
    info.device_id = 0x2F04;
    info.driver_version = (uint64_t(32) << 48) | (uint64_t(0) << 32) | (uint64_t(15) << 16) | 8142;
    info.tiled_resources_tier = 3;
    info.sample_positions_tier = 2;
    info.rov_supported = true;
    info.host_2x = render_target_cache->host_2x;
    info.quality_2x[0] = 1;
    info.quality_4x[0] = 1;
    info.resolution_scale_x = texture_cache->scale;
    info.resolution_scale_y = texture_cache->scale;
    info.async_shader_compilation = true;
    info.stage_a_bool = kAa838StageAByte1ZeroBool;
    info.stage_a_ps = kAa838InterleavedCopyPixelShader;
    telemetry.LogDevice(info);
  }

  // The order of the real SetupContext; the device line comes last.
  bool Setup(bool texture_cache_fails) {
    render_target_cache = std::make_unique<RenderTargetCache>();
    if (texture_cache_fails) {
      return false;
    }
    texture_cache = std::make_unique<TextureCache>();
    LogDevice();
    return true;
  }

  void Shutdown() {
    telemetry.Flush("exit");
    texture_cache.reset();
    render_target_cache.reset();
  }
};

void TestDeviceLineLifecycle() {
  std::printf("device line: never before the caches exist, once after, partial setup and shutdown are safe\n");
  Capture early;
  FakeCommandProcessor partial{early.telemetry};
  partial.LogDevice();
  partial.render_target_cache = std::make_unique<FakeCommandProcessor::RenderTargetCache>();
  partial.LogDevice();
  CHECK(early.CountContaining("AA838 device") == 0);
  CHECK(!partial.Setup(true));
  CHECK(early.CountContaining("AA838 device") == 0);
  partial.Shutdown();
  partial.Shutdown();
  CHECK(early.CountContaining("summary reason=exit frames=0 aa838_frames=0 draws=0") == 1);
  CHECK(early.lines.size() == 1);

  Capture full;
  FakeCommandProcessor processor{full.telemetry};
  CHECK(processor.Setup(false));
  CHECK(full.CountContaining("DEBUG_SAULO AA838 device vendor=0x10DE device=0x2F04 driver=32.0.15.8142 "
                             "tiled_tier=3 unaligned_bc=0 sample_positions_tier=2 rov_supported=1 bindless=0 "
                             "rt_path=rtv native_2x_msaa=0 host_2x=0 guest2x_on=host4x_samples0+3_mask0x9 "
                             "sample_positions=driver_default sample_quality=0 "
                             "q2x=[rgba8:1 rgb10a2:0 d24s8:0 d32s8:0 r32u:0] "
                             "q4x=[rgba8:1 rgb10a2:0 d24s8:0 d32s8:0 r32u:0] scale=2x2 "
                             "async_shader_compilation=1") == 1);
  CHECK(full.CountContaining("stage_a_bool=120 stage_a_ps=AA83877EDB2163EE") == 1);
  FakeCommandProcessor second{full.telemetry};
  CHECK(second.Setup(false));
  processor.LogDevice();
  CHECK(full.CountContaining("AA838 device") == 1);

  Aa838StageAFix fix;
  RunFrame(fix, full.telemetry, 1, {A, A, A});
  CHECK(full.CountContaining("ordinal=0 stage=A") == 1);
  CHECK(full.CountContaining("anomaly") == 0);
  processor.Shutdown();
  CHECK(full.CountContaining("summary reason=exit frames=1 aa838_frames=1 draws=3") == 1);
}

// The crash of the first 0088: the device line read texture_cache_ right after the render target cache was
// initialized, before SetupContext created the texture cache.
void TestDeviceLinePlacement() {
  std::printf("device line: the patch calls it last in SetupContext, behind a readiness guard\n");
  std::string patch = ReadPatch("0088-debug-saulo-aa838-telemetry.windows.patch");
  std::string cp = AddedLines(patch, "src/graphics/d3d12/command_processor.cpp");
  CHECK(Count(cp, "  DebugSauloAa838LogDevice();") == 1);
  CHECK(Has(patch, "+  DebugSauloAa838LogDevice();\n   return true;\n }\n \n"
                   " void D3D12CommandProcessor::ShutdownContext() {\n"));
  CHECK(!Has(patch, "Failed to initialize the render target cache\");\n     return false;\n   }\n"
                    "+  DebugSauloAa838LogDevice();"));
  size_t body = cp.find("void D3D12CommandProcessor::DebugSauloAa838LogDevice() {\n");
  CHECK(body != std::string::npos);
  const std::string guarded =
      "void D3D12CommandProcessor::DebugSauloAa838LogDevice() {\n"
      "  if (!render_target_cache_ || !texture_cache_) {\n    return;\n  }\n";
  CHECK(cp.compare(body, guarded.size(), guarded) == 0);
  size_t end = cp.find("\n}\n", body);
  size_t guard = body + guarded.size();
  for (const char* read : {"texture_cache_->", "render_target_cache_->"}) {
    for (size_t at = cp.find(read, body); at != std::string::npos && at < end; at = cp.find(read, at + 1)) {
      CHECK(at > guard);
    }
  }
}

void TestBoolSlot() {
  std::printf("bool 120: the uploaded word/bit is the one the translated shader reads\n");
  CHECK(kAa838StageAByte1ZeroBool == 120);
  CHECK((kAa838StageAByte1ZeroBool >> 5) == 3);
  CHECK((kAa838StageAByte1ZeroBool & 31) == 24);
  CHECK((kAa838StageAByte1ZeroBool >> 7) == 0);
  CHECK(((kAa838StageAByte1ZeroBool >> 5) & 3) == 3);
  std::string fix = ReadPatch("0080-tmnt-aa838-stage-a-fix.windows.patch");
  std::string cp = AddedLines(fix, "src/graphics/d3d12/command_processor.cpp");
  CHECK(Has(cp, "reinterpret_cast<uint32_t*>(bool_loop_constants)[kAa838StageAByte1ZeroBool >> 5];"));
  CHECK(Has(cp, "word |= UINT32_C(1) << (kAa838StageAByte1ZeroBool & 31);"));
  std::string om = AddedLines(fix, "src/graphics/pipeline/shader/dxbc_translator_om.cpp");
  CHECK(Has(om, "kAa838StageAByte1ZeroBool >> 7)"));
  CHECK(Has(om, ".Select((kAa838StageAByte1ZeroBool >> 5) & 3)"));
  CHECK(Has(om, "dxbc::Src::LU(UINT32_C(1) << (kAa838StageAByte1ZeroBool & 31))"));
  CHECK(Has(om, "a_.OpMov(dxbc::Dest::R(system_temps_color_[0], 0b0010), dxbc::Src::LF(0.0f));"));

  std::string telemetry = ReadPatch("0088-debug-saulo-aa838-telemetry.windows.patch");
  std::string tcp = AddedLines(telemetry, "src/graphics/d3d12/command_processor.cpp");
  CHECK(Has(tcp, "(reinterpret_cast<const uint32_t*>(bool_loop_constants)[kAa838StageAByte1ZeroBool >> 5] >>"));
  CHECK(Has(tcp, "(kAa838StageAByte1ZeroBool & 31)) &"));
}

void TestTelemetryPatchIsTelemetryOnly() {
  std::printf("0088: telemetry only, AA838 only\n");
  std::string patch = ReadPatch("0088-debug-saulo-aa838-telemetry.windows.patch");
  CHECK(!patch.empty());
  std::string cp = AddedLines(patch, "src/graphics/d3d12/command_processor.cpp");
  std::string om = AddedLines(patch, "src/graphics/pipeline/shader/dxbc_translator_om.cpp");
  std::string tc = AddedLines(patch, "src/graphics/d3d12/texture_cache.cpp");
  CHECK(!cp.empty() && !om.empty() && !tc.empty());
  for (const std::string* added : {&cp, &om, &tc}) {
    CHECK(Count(*added, "aa838_stage_a_pending_ =") == 0);
    CHECK(Count(*added, "aa838_stage_a_bool_state_ =") == 0);
    CHECK(Count(*added, "up_to_date") == 0);
    CHECK(Count(*added, "word |=") == 0);
    CHECK(Count(*added, "a_.Op") == 0);
    CHECK(Count(*added, "deferred_command_list_") == 0);
    for (const char* rendering : {"aa838_stage_a_fix_.", "kAa838StageAByte1ZeroBool =", "SampleDesc", "SampleMask",
                                  "SetSamplePositions", "msaa_2x_supported_ =", "path_ =", "ConfigurePipeline",
                                  "SetPipelineState", "D3DDraw", "SetPrimitiveTopology(", "RequestTextures(",
                                  "render_target_cache_->Update(", "system_temps_color_", "edram_rov_used_ ="}) {
      CHECK(Count(*added, rendering) == 0);
    }
  }
  for (const char* untouched : {"aa838_stage_fix.h", "aa838_stage_fix.cpp", "include/rex/graphics/flags.h",
                                "pipeline_cache.cpp", "render_target_cache.cpp", "deferred_command_list"}) {
    CHECK(!Has(patch, (std::string("+++ b/") + "src/graphics/d3d12/" + untouched).c_str()));
    CHECK(!Has(patch, (std::string("+++ b/") + untouched).c_str()));
    CHECK(!Has(patch, (std::string("+++ b/include/rex/graphics/d3d12/") + untouched).c_str()));
  }
  CHECK(CountStatements(cp, "return") == 2);
  CHECK(CountStatements(om, "return ") == 0);
  CHECK(CountStatements(tc, "return ") == 0);
  CHECK(Has(cp, "if (pixel_shader && pixel_shader->ucode_data_hash() == kAa838InterleavedCopyPixelShader) {\n"
                "    aa838_trace.active = true;"));
  CHECK(Has(om, "if (is_pixel_shader() && current_shader().ucode_data_hash() == kAa838InterleavedCopyPixelShader)"));
  CHECK(Has(om, "!edram_rov_used_ && IsAa838Shader() && current_shader().writes_color_target(0)"));
  CHECK(Has(cp, "DebugSauloAa838TelemetryInstance().OnFrameEnd(frame_current_);"));
  CHECK(Has(cp, "DebugSauloAa838TelemetryInstance().Flush(\"exit\");"));
  CHECK(Has(tc, "logged.fetch_add(1, std::memory_order_relaxed) < 16"));
  CHECK(Count(patch, "DEBUG_SAULO") >= 8);
  std::string sink = AddedLines(patch, "src/graphics/d3d12/debug_saulo_aa838_telemetry.cpp");
  CHECK(Has(sink, "REXGPU_WARN(\"{}\", line)"));
}

}  // namespace

int main() {
  TestClassifierNormalSequence();
  TestClassifierFrameReset();
  TestClassifierIgnoresOtherShaders();
  TestClassifierExtraDraws();
  TestTelemetryNormalSequence();
  TestTelemetryFrameReset();
  TestTelemetryFourthDraw();
  TestTelemetryTwoSequences();
  TestTelemetryShortFrame();
  TestTelemetryStageAAnomalies();
  TestTelemetryDroppedAndUncompiled();
  TestTelemetryMsaa();
  TestTelemetryAlternatingState();
  TestTelemetryCaps();
  TestTelemetrySummary();
  TestDeviceLineLifecycle();
  TestDeviceLinePlacement();
  TestBoolSlot();
  TestTelemetryPatchIsTelemetryOnly();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
