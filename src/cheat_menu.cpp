#include "cheat_menu.h"

#include <cstddef>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include "cheats.h"
#include "keybinds.h"
#include "launcher.h"
#include "localization/ui_text.h"
#include "modal_dialog.h"

namespace tmnt {
namespace {

// Above the ImGui drawer (64), so F10 is consumed before the drawer or the game see it.
constexpr size_t kInputZOrder = 1000;

const ImVec4 kAccent(0.36f, 0.88f, 0.44f, 1.0f);
const ImVec4 kPanel(0.04f, 0.08f, 0.07f, 0.92f);
const ImVec4 kRowFocus(0.09f, 0.36f, 0.22f, 0.95f);
const ImVec4 kDim(0.62f, 0.68f, 0.66f, 1.0f);

}  // namespace

class CheatMenuDialog final : public ModalDialog {
 public:
  CheatMenuDialog(rex::ui::ImGuiDrawer* drawer, CheatMenu& menu) : ModalDialog(drawer), menu_(menu) {}
  ~CheatMenuDialog() override { menu_.DialogClosed(); }

 protected:
  void DrawContent(ImGuiIO& io) override {
    ReadKeys();
    DrawMenu(io);
  }

 private:
  void Press(cheats::MenuKey key) {
    const cheats::CheatMenuModel::Result result = menu_.model_.Press(key, true);
    if (result.action != cheats::CheatMenuModel::Action::kChange) {
      return;
    }
    const cheats::CheatMenuRow& row = cheats::kCheatMenuRows[result.row];
    if (row.multiplier) {
      cheats::CycleDamageMultiplierForSession();
    } else {
      cheats::ToggleForSession(row.cheat);
    }
  }

  void ReadKeys() {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) {
      Press(cheats::MenuKey::kUp);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) {
      Press(cheats::MenuKey::kDown);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      Press(cheats::MenuKey::kEnter);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
      Press(cheats::MenuKey::kPadA);
    }
  }

  void DrawMenu(ImGuiIO& io) {
    ImFont* regular = LauncherRegularFont();
    ImFont* strong = LauncherSemiboldFont() ? LauncherSemiboldFont() : regular;
    ImGui::PushFont(regular);
    const float em = ImGui::GetFontSize();
    const float margin = em * 0.8f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(em * 21.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##tmnt_cheat_menu", nullptr, kFlags)) {
      ImGui::PushFont(strong);
      ImGui::TextColored(kAccent, "%s", Tr(cheats::kCheatMenuTitle));
      ImGui::PopFont();
      ImGui::Separator();
      for (size_t i = 0; i < cheats::kCheatMenuRowCount; ++i) {
        if (i == cheats::kCheatMenuPlayerRow || i == cheats::kCheatMenuCombatRow) {
          ImGui::TextColored(kDim, "%s", Tr(i == cheats::kCheatMenuPlayerRow ? "PLAYER" : "COMBAT"));
        }
        DrawRow(cheats::kCheatMenuRows[i], i, io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
      }
      ImGui::Separator();
      DrawSpinAttackHelp();
      ImGui::TextColored(kDim, "%s", Tr(cheats::kCheatMenuCloseHint));
      ImGui::TextColored(kDim, "%s", Tr(cheats::kCheatMenuNote));
      ImGui::TextColored(kDim, "%s", Tr(cheats::kCheatMenuOnlineNote));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
  }

  void DrawSpinAttackHelp() {
    const cheats::CheatMenuRow& row = cheats::kCheatMenuRows[menu_.model_.selected()];
    if (row.multiplier || row.cheat != cheats::Cheat::SpinAttack) {
      return;
    }
    const std::string binding =
        UsableKeyboardBinding(rex::cvar::GetFlagByName(kSpinAttackKeyAction.key), kSpinAttackKeyAction);
    ImGui::TextColored(kAccent, "%s", Tr(cheats::kSpinAttackControllerHelp));
    ImGui::TextColored(kAccent, "%s %s", Tr(cheats::kSpinAttackKeyboardHelp),
                       TrMessage(KeyboardBindingLabel(binding)).c_str());
    ImGui::Separator();
  }

  void DrawRow(const cheats::CheatMenuRow& row, size_t index, bool mouse_moved) {
    const int multiplier = row.multiplier ? cheats::DamageMultiplier() : 0;
    const bool on = row.multiplier ? multiplier > 1 : cheats::Enabled(row.cheat);
    const float height = ImGui::GetFrameHeight() + 4.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + height);
    ImGui::Dummy(ImVec2(max.x - min.x, height));
    if (ImGui::IsMouseHoveringRect(min, max)) {
      if (mouse_moved) menu_.model_.Select(index);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        menu_.model_.Select(index);
        Press(cheats::MenuKey::kEnter);
      }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (menu_.model_.selected() == index) {
      draw->AddRectFilled(min, max, ImGui::GetColorU32(kRowFocus), 3.0f);
    }
    const float pad = ImGui::GetFontSize() * 0.4f;
    const float y = min.y + (height - ImGui::GetFontSize()) * 0.5f;
    const char* state = row.multiplier ? Tr(cheats::DamageMultiplierLabel(multiplier)) : Tr(on ? "On" : "Off");
    draw->AddText(ImVec2(min.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text), Tr(row.label));
    draw->AddText(ImVec2(max.x - pad - ImGui::CalcTextSize(state).x, y), ImGui::GetColorU32(on ? kAccent : kDim),
                  state);
  }

  CheatMenu& menu_;
};

void CheatMenu::Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  window_ = window;
  drawer_ = drawer;
  window_->AddInputListener(this, kInputZOrder);
}

void CheatMenu::Detach() {
  if (window_) {
    window_->RemoveInputListener(this);
    window_ = nullptr;
  }
  delete dialog_;
  dialog_ = nullptr;
  drawer_ = nullptr;
}

void CheatMenu::OnKeyDown(rex::ui::KeyEvent& e) {
  if (e.target() != window_ || e.virtual_key() != rex::ui::VirtualKey::kF10) {
    return;
  }
  e.set_handled(true);
  if (e.prev_state()) {
    return;
  }
  const bool can_open = cheats::CanOpenCheatMenu(drawer_ != nullptr, ModalDialog::AnyOpen());
  const cheats::CheatMenuModel::Result result = model_.Press(cheats::MenuKey::kF10, can_open);
  if (result.action == cheats::CheatMenuModel::Action::kOpen) {
    dialog_ = new CheatMenuDialog(drawer_, *this);
  } else if (result.action == cheats::CheatMenuModel::Action::kClose && dialog_) {
    dialog_->RequestClose();
  }
}

void CheatMenu::DialogClosed() {
  dialog_ = nullptr;
  model_.Closed();
}

}  // namespace tmnt
