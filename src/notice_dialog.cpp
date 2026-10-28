#include "notice_dialog.h"

#include <utility>

#include "launcher.h"

namespace tmnt {
namespace {

const ImVec4 kAccent(0.36f, 0.88f, 0.44f, 1.0f);
const ImVec4 kPanel(0.04f, 0.08f, 0.07f, 0.94f);
const ImVec4 kButtonFocus(0.09f, 0.36f, 0.22f, 0.95f);
const ImVec4 kDim(0.62f, 0.68f, 0.66f, 1.0f);

}  // namespace

NoticeDialog::NoticeDialog(rex::ui::ImGuiDrawer* drawer, std::string title, std::vector<std::string> lines,
                           std::function<void()> closed)
    : ModalDialog(drawer), title_(std::move(title)), lines_(std::move(lines)), closed_(std::move(closed)) {}

NoticeDialog::~NoticeDialog() {
  if (closed_) closed_();
}

void NoticeDialog::DrawContent(ImGuiIO& io) {
  // The press that opened the notice may still be down: OK answers only after A and Enter were let go once.
  if (!armed_ && !ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) && !ImGui::IsKeyDown(ImGuiKey_Enter) &&
      !ImGui::IsKeyDown(ImGuiKey_KeypadEnter)) {
    armed_ = true;
  }
  ImFont* regular = LauncherRegularFont();
  ImFont* strong = LauncherSemiboldFont() ? LauncherSemiboldFont() : regular;
  ImGui::PushFont(regular);
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(em * 28.0f, 0.0f), ImGuiCond_Always);
  if (!focused_) {
    focused_ = true;
    ImGui::SetNextWindowFocus();
  }
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Button, kButtonFocus);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kButtonFocus);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonFocus);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.9f, em * 0.7f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##tmnt_notice", nullptr, kFlags)) {
    ImGui::PushFont(strong);
    ImGui::TextColored(kAccent, "%s", title_.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    for (const std::string& line : lines_) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(em * 6.0f, 0.0f)) && armed_) {
      RequestClose();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "A  OK     B  Close");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(5);
  ImGui::PopFont();
}

}  // namespace tmnt
