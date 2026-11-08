#pragma once

#include <rex/ui/window_listener.h>

#include "cheat_menu_rules.h"

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace tmnt {

class CheatMenuDialog;

// The in-game cheat menu, opened and closed with F10. It changes the cheats for this session only; the
// launcher's saved settings are never written from here.
class CheatMenu final : public rex::ui::WindowInputListener {
 public:
  void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);
  void Detach();

 private:
  friend class CheatMenuDialog;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void DialogClosed();

  rex::ui::Window* window_ = nullptr;
  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  CheatMenuDialog* dialog_ = nullptr;
  cheats::CheatMenuModel model_;
};

}  // namespace tmnt
