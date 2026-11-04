#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace tmnt {

// DEBUG_SAULO: sampling of the frame thread: needs TMNT_FRAME_BLOCK_SAMPLER set, and runs only while ticks are slow.
bool FrameSamplerEnabled();
void NoteFrameThreadForSampler(PPCContext* ctx, uint8_t* base);
void SetFrameSamplerActive(bool active);
// DEBUG_SAULO: "parked lock=.. event=.. ... sites wait@lr<caller<caller=samples thread_cpu_ms name=ms", then resets.
std::string FrameBlockSummary();

}  // namespace tmnt
