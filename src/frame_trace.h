#pragma once

#include <cstdint>
#include <string>

namespace tmnt {

// DEBUG_SAULO: traces the next frames of the game tick, stage by stage.
void ArmFrameTrace(int frames);

// DEBUG_SAULO: the game stages that took longest since the last call, as "name[address]=ms,..." (nested stages overlap).
std::string FrameStageSummary();
// DEBUG_SAULO: CPU time of the calling thread and of the whole process.
uint64_t ThreadCpuMicroseconds();
uint64_t ProcessCpuMicroseconds();

}  // namespace tmnt
