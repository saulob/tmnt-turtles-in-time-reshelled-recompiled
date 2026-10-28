#pragma once

struct PPCFuncMapping;

namespace tmnt {

// Registers the generated guest-to-host function table, so a native crash address can name the recompiled function.
void InstallGuestFunctionTable(const PPCFuncMapping* mappings);

// Samples the descriptor C state for the crash report. Called once per UI frame; cheap and passive.
void SampleCrashGuestState();

}  // namespace tmnt
