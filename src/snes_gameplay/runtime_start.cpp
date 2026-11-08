#include "snes_gameplay/runtime_start.h"

#include <cstdio>

namespace tmnt::snes {

RuntimeStart PrepareRuntimeStart(const GameplayStatusResult& status) {
  RuntimeStart start;
  if (status.status != GameplayStatus::kReady) {
    start.message = status.status == GameplayStatus::kNotImported
                        ? "SNES Gameplay: unavailable, SNES ROM not imported"
                        : "SNES Gameplay: unavailable, " + status.detail;
    start.warning = status.status != GameplayStatus::kNotImported;
    return start;
  }
  const RuntimeData runtime = ToRuntimeData(status.data);
  start.data = EncodeRuntimeData(runtime);
  char timing[96];
  std::snprintf(timing, sizeof timing, "%.2f Hz, dash after %u frames (%.2f s)", runtime.frame_rate,
                runtime.dash_hold_frames, runtime.Seconds(runtime.dash_hold_frames));
  start.message = "SNES Gameplay: Ready, source " + status.region + ", " + timing;
  return start;
}

}  // namespace tmnt::snes
