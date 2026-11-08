#pragma once

#include <rex/ui/window_listener.h>

#include "snes_menu_rules.h"

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace tmnt {

class SnesMenuDialog;

// The in-game SNES menu, opened and closed with F9. It changes the SNES features for this session only; the
// launcher's saved settings are never written from here.
class SnesMenu final : public rex::ui::WindowInputListener {
 public:
  void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);
  void Detach();

 private:
  friend class SnesMenuDialog;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void DialogClosed();

  rex::ui::Window* window_ = nullptr;
  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  SnesMenuDialog* dialog_ = nullptr;
  snes::MenuModel model_;
};

}  // namespace tmnt
