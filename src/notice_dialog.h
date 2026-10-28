#pragma once

#include <functional>
#include <string>
#include <vector>

#include "modal_dialog.h"

namespace tmnt {

// A small message with an OK button, for what the original Xbox 360 menus ask of services the port does not
// have. It deletes itself when closed and calls `closed` as it goes.
class NoticeDialog final : public ModalDialog {
 public:
  NoticeDialog(rex::ui::ImGuiDrawer* drawer, std::string title, std::vector<std::string> lines,
               std::function<void()> closed);
  ~NoticeDialog() override;

 protected:
  void DrawContent(ImGuiIO& io) override;

 private:
  std::string title_;
  std::vector<std::string> lines_;
  std::function<void()> closed_;
  bool focused_ = false;
  bool armed_ = false;
};

}  // namespace tmnt
