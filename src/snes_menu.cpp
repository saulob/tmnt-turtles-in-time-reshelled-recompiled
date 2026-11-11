#include "snes_menu.h"

#include <cstddef>

#include <imgui.h>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include "launcher.h"
#include "localization/ui_text.h"
#include "modal_dialog.h"
#include "snes_gameplay_runtime.h"

namespace tmnt {
namespace {

// Above the ImGui drawer (64), so F9 is consumed before the drawer or the game see it.
constexpr size_t kInputZOrder = 1000;

const ImVec4 kAccent(0.36f, 0.88f, 0.44f, 1.0f);
const ImVec4 kPanel(0.04f, 0.08f, 0.07f, 0.92f);
const ImVec4 kRowFocus(0.09f, 0.36f, 0.22f, 0.95f);
const ImVec4 kDim(0.62f, 0.68f, 0.66f, 1.0f);
const ImVec4 kDisabled(0.42f, 0.46f, 0.45f, 1.0f);

}  // namespace

class SnesMenuDialog final : public ModalDialog {
 public:
  SnesMenuDialog(rex::ui::ImGuiDrawer* drawer, SnesMenu& menu) : ModalDialog(drawer), menu_(menu) {}
  ~SnesMenuDialog() override { menu_.DialogClosed(); }

 protected:
  void DrawContent(ImGuiIO& io) override {
    available_ = SnesAvailability();
    online_ = SnesOnline();
    ReadKeys();
    DrawMenu(io);
  }

 private:
  void Press(snes::MenuKey key) {
    const snes::MenuModel::Result result = menu_.model_.Press(key, true, available_);
    if (result.action == snes::MenuModel::Action::kChange) {
      const snes::Feature feature = snes::MenuRow(result.row).feature;
      if (feature == snes::Feature::kDash) {
        if (snes::DashRowState(online_, true, snes::DashMode::kAuto).usable) StepSnesDashForSession(result.step);
      } else if (snes::RowStateOf(online_, feature, true, false).usable) {
        ToggleSnesFeatureForSession(feature);
      }
    }
  }

  void ReadKeys() {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) {
      Press(snes::MenuKey::kUp);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) {
      Press(snes::MenuKey::kDown);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) {
      Press(snes::MenuKey::kLeft);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) {
      Press(snes::MenuKey::kRight);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      Press(snes::MenuKey::kEnter);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
      Press(snes::MenuKey::kPadA);
    }
  }

  void DrawMenu(ImGuiIO& io) {
    ImFont* regular = LauncherRegularFont();
    ImFont* strong = LauncherSemiboldFont() ? LauncherSemiboldFont() : regular;
    ImGui::PushFont(regular);
    const float em = ImGui::GetFontSize();
    const float margin = em * 0.8f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(em * 24.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##tmnt_snes_menu", nullptr, kFlags)) {
      ImGui::PushFont(strong);
      ImGui::TextColored(kAccent, "%s", Tr(snes::kMenuTitle));
      ImGui::PopFont();
      ImGui::Separator();
      const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
      for (size_t i = 0; i < snes::kMenuRowCount; ++i) {
        if (i == snes::kMenuAudioRow || i == snes::kMenuGameplayRow) {
          ImGui::TextColored(kDim, "%s", Tr(i == snes::kMenuAudioRow ? snes::kMenuAudioSection : snes::kMenuGameplaySection));
        }
        DrawRow(snes::MenuRow(i), i, mouse_moved);
      }
      ImGui::TextColored(kDim, "%s", Tr(snes::OnlineNote(online_)));
      ImGui::Separator();
      if (const char* status = snes::StatusMessage(available_)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kAccent, "%s", Tr(status));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
      }
      ImGui::TextColored(kDim, "%s", Tr(snes::kMenuCloseHint));
      ImGui::TextColored(kDim, "%s", Tr(snes::kMenuNote));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
  }

  void DrawRow(const snes::FeatureInfo& row, size_t index, bool mouse_moved) {
    const bool local = snes::RowAvailable(available_, row.feature);
    const snes::RowState shown =
        row.feature == snes::Feature::kDash
            ? snes::DashRowState(online_, local, SnesDashMode())
            : snes::RowStateOf(online_, row.feature, local, local && SnesFeatureEnabled(row.feature));
    const bool available = shown.usable;
    const float height = ImGui::GetFrameHeight() + 4.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + height);
    ImGui::Dummy(ImVec2(max.x - min.x, height));
    if (ImGui::IsMouseHoveringRect(min, max)) {
      if (mouse_moved) menu_.model_.Select(index);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        menu_.model_.Select(index);
        Press(snes::MenuKey::kEnter);
      }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (menu_.model_.selected() == index) {
      draw->AddRectFilled(min, max, ImGui::GetColorU32(kRowFocus), 3.0f);
    }
    const float pad = ImGui::GetFontSize() * 0.4f;
    const float y = min.y + (height - ImGui::GetFontSize()) * 0.5f;
    const char* state = Tr(shown.text);
    const bool lit = shown.lit;
    const ImU32 label_color = available ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(kDisabled);
    const ImU32 state_color = ImGui::GetColorU32(!available ? kDisabled : (lit ? kAccent : kDim));
    draw->AddText(ImVec2(min.x + pad, y), label_color, Tr(row.label));
    draw->AddText(ImVec2(max.x - pad - ImGui::CalcTextSize(state).x, y), state_color, state);
    if (shown.reason) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(kDim, "%s", Tr(shown.reason));
      ImGui::PopTextWrapPos();
    }
  }

  SnesMenu& menu_;
  snes::Availability available_;
  snes::OnlineView online_;
};

void SnesMenu::Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  window_ = window;
  drawer_ = drawer;
  window_->AddInputListener(this, kInputZOrder);
}

void SnesMenu::Detach() {
  if (window_) {
    window_->RemoveInputListener(this);
    window_ = nullptr;
  }
  delete dialog_;
  dialog_ = nullptr;
  drawer_ = nullptr;
}

void SnesMenu::OnKeyDown(rex::ui::KeyEvent& e) {
  if (e.target() != window_ || e.virtual_key() != rex::ui::VirtualKey::kF9) {
    return;
  }
  e.set_handled(true);
  if (e.prev_state()) {
    return;
  }
  const bool can_open = snes::CanOpenMenu(drawer_ != nullptr, ModalDialog::AnyOpen());
  const snes::MenuModel::Result result = model_.Press(snes::MenuKey::kF9, can_open, SnesAvailability());
  if (result.action == snes::MenuModel::Action::kOpen) {
    dialog_ = new SnesMenuDialog(drawer_, *this);
  } else if (result.action == snes::MenuModel::Action::kClose && dialog_) {
    dialog_->RequestClose();
  }
}

void SnesMenu::DialogClosed() {
  dialog_ = nullptr;
  model_.Closed();
}

}  // namespace tmnt
