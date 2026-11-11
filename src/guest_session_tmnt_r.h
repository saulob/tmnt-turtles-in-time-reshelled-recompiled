#pragma once

#include <cstdint>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>

REX_EXTERN(__imp__sub_87520018);

namespace tmnt {

inline constexpr uint32_t kImportIsInSession = 1145;  // ONL_IsInSession, the scripts' IS_ALONE

// An engine import resolved as the compiled scripts do (sub_87520018 returns the function behind an index).
inline PPCFunc* ResolveGuestImport(rex::CallFrame& frame, uint8_t* base, uint32_t index) {
  frame.ctx.r3.u64 = index;
  __imp__sub_87520018(frame.ctx, base);
  return rex::runtime::ResolveIndirectFunction(frame.ctx.r3.u32);
}

// A side call, so the hooked function's own registers stay as the game passed them. Unresolved counts as online.
inline bool GuestInOnlineSession(PPCContext& parent, uint8_t* base) {
  rex::CallFrame frame(parent);
  frame.ctx.lr = parent.lr;
  PPCFunc* is_in_session = ResolveGuestImport(frame, base, kImportIsInSession);
  if (!is_in_session) {
    return true;
  }
  is_in_session(frame.ctx, base);
  return frame.ctx.r3.u32 != 0;
}

}  // namespace tmnt
