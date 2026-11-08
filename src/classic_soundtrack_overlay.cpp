#include "classic_soundtrack_overlay.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "classic_soundtrack_notice.h"
#include "classic_soundtrack_runtime.h"
#include "corner_overlay.h"
#include "frame_counter.h"
#include "settings.h"

namespace tmnt {
namespace {

SoundtrackNoticeState g_notice;

class ClassicSoundtrackOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit ClassicSoundtrackOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    const SoundtrackNotice notice = g_notice.Current(ImGui::GetTime());
    if (notice == SoundtrackNotice::kNone) {
      return;
    }
    const bool counter = FrameCounterVisible(REXCVAR_GET(tmnt_show_frame_counter), GetGuestFrameStats());
    DrawCornerText(LocalizedSoundtrackNoticeText(notice), ClassicSoundtrackSlot(counter));
  }
};

void SwitchSoundtrack() {
  const classic::RuntimeToggle toggle = ToggleClassicSoundtrack();
  g_notice.Show(NoticeForToggle(toggle), kToggleNoticeSeconds);
  REXLOG_INFO("Classic soundtrack: {} key -> {}", kClassicSoundtrackKey, SoundtrackNoticeText(NoticeForToggle(toggle)));
}

}  // namespace

bool ClassicSoundtrackNoticeVisible() {
  return g_notice.Current(ImGui::GetTime()) != SoundtrackNotice::kNone;
}

void CreateClassicSoundtrackOverlay(rex::ui::ImGuiDrawer* drawer) {
  new ClassicSoundtrackOverlay(drawer);  // retained by the drawer for the session
  if (IsClassicSoundtrackActive()) {
    g_notice.Show(SoundtrackNotice::kOn, kStartupNoticeSeconds);
  }
  rex::ui::RegisterBind("bind_tmnt_classic_soundtrack", kClassicSoundtrackKey, "Switch SNES and original soundtrack",
                        [] { SwitchSoundtrack(); });
}

}  // namespace tmnt
