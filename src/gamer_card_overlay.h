#pragma once

#include <functional>

#include "gamer_card_rules.h"
#include "modal_dialog.h"

namespace tmnt {

// The local Gamer Card, styled and sized like the achievements list, with a B Back footer. It deletes itself when
// closed and calls `closed` as it goes.
class GamerCardOverlay final : public ModalDialog {
 public:
  GamerCardOverlay(rex::ui::ImGuiDrawer* drawer, GamerCardView view, std::function<void()> closed);
  ~GamerCardOverlay() override;

 protected:
  void DrawContent(ImGuiIO& io) override;

 private:
  GamerCardView view_;
  std::function<void()> closed_;
  bool focused_ = false;
};

}  // namespace tmnt
