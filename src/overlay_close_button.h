#pragma once

#include <imgui.h>

#include <rex/ui/style.h>

namespace tmnt {

constexpr float kOverlayCloseButtonSize = 22.0f;

// A square X at the right end of the first header row; `id` makes the button unique within its window.
inline bool DrawOverlayCloseButton(const rex::ui::AchievementsStyle& style, float row_y, const char* id) {
  const float button_y = row_y + (ImGui::GetTextLineHeight() - kOverlayCloseButtonSize) * 0.5f - 2.0f;
  const ImVec2 restore = ImGui::GetCursorPos();
  ImGui::SetCursorPos(
      ImVec2(ImGui::GetWindowWidth() - style.window_padding.x - kOverlayCloseButtonSize, button_y));
  const ImVec4& tint = style.progress_bar;
  ImGui::PushStyleColor(ImGuiCol_Text, tint);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tint.x, tint.y, tint.z, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tint.x, tint.y, tint.z, 0.50f));
  ImGui::PushStyleColor(ImGuiCol_Border, tint);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  const bool clicked = ImGui::Button(id, ImVec2(kOverlayCloseButtonSize, kOverlayCloseButtonSize));
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
  ImGui::SetCursorPos(restore);
  return clicked;
}

}  // namespace tmnt
