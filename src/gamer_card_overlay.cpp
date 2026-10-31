#include "gamer_card_overlay.h"

#include <utility>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/style.h>

#include "launcher.h"
#include "overlay_close_button.h"

namespace tmnt {
namespace {

constexpr float kMinWidth = 360.0f;
constexpr float kMaxWidth = 460.0f;
constexpr float kMaxHeight = 420.0f;
constexpr float kNameScale = 1.5f;
constexpr float kHintScale = 1.2f;
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize;

void DrawRows(const rex::ui::AchievementsStyle& style, const std::vector<GamerCardRow>& rows, const char* table_id) {
  if (rows.empty() || !ImGui::BeginTable(table_id, 2, ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  for (const GamerCardRow& row : rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(style.locked_title, "%s", row.label.c_str());
    ImGui::TableNextColumn();
    ImGui::TextColored(style.badge_gamerscore, "%s", row.value.c_str());
  }
  ImGui::EndTable();
}

}  // namespace

GamerCardOverlay::GamerCardOverlay(rex::ui::ImGuiDrawer* drawer, GamerCardView view, std::function<void()> closed)
    : ModalDialog(drawer), view_(std::move(view)), closed_(std::move(closed)) {
  SetGamepadCloseEnabled(false);
}

GamerCardOverlay::~GamerCardOverlay() {
  if (closed_) closed_();
}

void GamerCardOverlay::DrawContent(ImGuiIO& io) {
  const auto& style = imgui_drawer()->style().achievements;
  const ImVec2 viewport = io.DisplaySize;
  ImGui::PushFont(LauncherRegularFont());
  ImGui::SetNextWindowPos(ImVec2(viewport.x * 0.5f, viewport.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  const float width = FitWindowExtent(viewport.x, kMinWidth, kMaxWidth);
  const float max_height = FitWindowExtent(viewport.y, 0.0f, kMaxHeight);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, max_height));
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (!focused_) {
    focused_ = true;
    ImGui::SetNextWindowFocus();
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.window_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, style.item_spacing);
  if (ImGui::Begin("Gamer Card##overlay", nullptr, kWindowFlags)) {
    const float header_row_y = ImGui::GetCursorPosY();
    ImGui::TextColored(style.header_text, "GAMER CARD");
    if (DrawOverlayCloseButton(style, header_row_y, "X##close_gamer_card")) {
      RequestClose();
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (view_.available) {
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * kNameScale);
      ImGui::TextColored(style.unlocked_title, "%s", view_.gamertag.c_str());
      ImGui::PopFont();
    }
    ImGui::TextColored(view_.available ? style.unlocked_desc : style.locked_desc, "%s", view_.status.c_str());
    ImGui::Spacing();

    DrawRows(style, view_.profile_rows, "##gamer_card_profile");

    if (view_.available) {
      ImGui::Separator();
      ImGui::TextColored(style.header_text, "%s", view_.game.c_str());
      DrawRows(style, view_.game_rows, "##gamer_card_game");
    }

    ImGui::Spacing();
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * kHintScale);
    ImGui::TextColored(style.header_text, "B  Back");
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

}  // namespace tmnt
