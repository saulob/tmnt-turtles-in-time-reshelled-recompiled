#include "ping_display.h"

#include <string>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "corner_overlay.h"
#include "frame_counter.h"
#include "online_runtime.h"
#include "settings.h"

REXCVAR_DECLARE(bool, tmnt_online_enabled);
REXCVAR_DECLARE(bool, tmnt_online_ping_display);

namespace tmnt {
namespace {

class PingOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PingOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!PingVisible(REXCVAR_GET(tmnt_online_enabled), REXCVAR_GET(tmnt_online_ping_display))) return;
    const bool counter = FrameCounterVisible(REXCVAR_GET(tmnt_show_frame_counter), GetGuestFrameStats());
    DrawCornerText(PingText(Online().WorstRtt()), PingSlot(counter));
  }
};

}  // namespace

void CreatePingDisplay(rex::ui::ImGuiDrawer* drawer) {
  new PingOverlay(drawer);  // retained by the drawer for the session
  rex::ui::RegisterBind("bind_tmnt_ping_display", kPingDisplayKey, "Toggle ping display", [] {
    if (!REXCVAR_GET(tmnt_online_enabled)) return;
    rex::cvar::SetFlagByName("tmnt_online_ping_display", REXCVAR_GET(tmnt_online_ping_display) ? "false" : "true");
  });
}

}  // namespace tmnt
