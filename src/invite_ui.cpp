#include "invite_ui.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

#include "launcher.h"
#include "localization/ui_text.h"
#include "modal_dialog.h"
#include "online/friend_view.h"
#include "online_runtime.h"

namespace tmnt {
namespace {

const ImVec4 kAccent(0.36f, 0.88f, 0.44f, 1.0f);
const ImVec4 kPanel(0.04f, 0.08f, 0.07f, 0.94f);
const ImVec4 kHighlight(0.09f, 0.36f, 0.22f, 0.95f);
const ImVec4 kDim(0.62f, 0.68f, 0.66f, 1.0f);

constexpr double kInviteCheckSeconds = 0.5;

rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::function<void(std::function<void()>)> g_post;
std::set<std::string> g_prompted;  // UI thread only

// Pushes the shared dialog style and opens a centered window; the caller ends it with EndStyledWindow.
bool BeginStyledWindow(const char* id, const ImGuiIO& io, bool* focused) {
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(em * 28.0f, 0.0f), ImGuiCond_Always);
  if (!*focused) {
    *focused = true;
    ImGui::SetNextWindowFocus();
  }
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Header, kHighlight);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHighlight);
  ImGui::PushStyleColor(ImGuiCol_Button, kHighlight);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.9f, em * 0.7f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoSavedSettings;
  return ImGui::Begin(id, nullptr, kFlags);
}

void EndStyledWindow() {
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(5);
}

class InvitePicker final : public ModalDialog {
 public:
  explicit InvitePicker(rex::ui::ImGuiDrawer* drawer) : ModalDialog(drawer) {}

 protected:
  void DrawContent(ImGuiIO& io) override {
    ImGui::PushFont(LauncherRegularFont());
    if (BeginStyledWindow("##tmnt_invite_picker", io, &focused_)) {
      ImGui::TextColored(kAccent, "%s", Tr("Invite friends"));
      ImGui::Separator();
      const online::Snapshot state = Online().snapshot();
      const std::vector<online::InviteChoice> choices = online::PartyInviteChoices(state);
      if (!state.party) ImGui::TextWrapped("%s", Tr("Create a match first, then invite friends."));
      else if (state.my_slot != 1) ImGui::TextWrapped("%s", Tr("Only the host of the match can invite friends."));
      else if (choices.empty()) ImGui::TextWrapped("%s", Tr("No friends yet. Add them on the launcher's Online page."));
      for (const online::InviteChoice& choice : choices) {
        ImGui::PushID(choice.player.c_str());
        const std::string row = choice.name + "    " + Tr(choice.status);
        const ImGuiSelectableFlags flags = choice.selectable ? 0 : ImGuiSelectableFlags_Disabled;
        if (ImGui::Selectable(row.c_str(), false, flags) && choice.selectable) Online().InviteToParty(choice.player);
        ImGui::PopID();
      }
      ImGui::Spacing();
      ImGui::TextColored(kDim, "%s", Tr("A  Invite     B  Back"));
    }
    EndStyledWindow();
    ImGui::PopFont();
  }

 private:
  bool focused_ = false;
};

class InvitePrompt final : public ModalDialog {
 public:
  InvitePrompt(rex::ui::ImGuiDrawer* drawer, online::Invite invite) : ModalDialog(drawer), invite_(std::move(invite)) {}

 protected:
  void DrawContent(ImGuiIO& io) override {
    if (!StillOffered()) {
      RequestClose();
      return;
    }
    ImGui::PushFont(LauncherRegularFont());
    if (BeginStyledWindow("##tmnt_invite_prompt", io, &focused_)) {
      ImGui::TextColored(kAccent, "%s", Tr("Online match invite"));
      ImGui::Separator();
      ImGui::TextWrapped(Tr("%s invited you to an online match."), invite_.from_name.c_str());
      ImGui::Spacing();
      if (ImGui::Button(Tr("Accept"))) {
        Online().AcceptPartyInvite(invite_.id);
        RequestClose();
      }
      ImGui::SameLine();
      if (ImGui::Button(Tr("Decline"))) {
        Online().DeclinePartyInvite(invite_.id);
        RequestClose();
      }
      ImGui::SameLine();
      ImGui::TextColored(kDim, "%s", Tr("A  Choose     B  Later"));
    }
    EndStyledWindow();
    ImGui::PopFont();
  }

 private:
  bool StillOffered() const {
    const std::vector<online::Invite> invites = Online().Invites();
    return std::any_of(invites.begin(), invites.end(), [this](const online::Invite& i) { return i.id == invite_.id; });
  }

  online::Invite invite_;
  bool focused_ = false;
};

// Opens the prompt once per invite, whenever no other dialog is up and the player is not already in a party.
class InviteWatcher final : public rex::ui::ImGuiDialog {
 public:
  explicit InviteWatcher(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    const double now = ImGui::GetTime();
    if (now < next_check_) return;
    next_check_ = now + kInviteCheckSeconds;
    if (ModalDialog::AnyOpen() || !OnlineRunning() || Online().party()) return;
    for (online::Invite& invite : Online().Invites()) {
      if (g_prompted.count(invite.id)) continue;
      g_post([invite = std::move(invite)]() mutable {
        if (ModalDialog::AnyOpen() || !g_prompted.insert(invite.id).second) return;
        new InvitePrompt(g_drawer, std::move(invite));
      });
      return;
    }
  }

 private:
  double next_check_ = 0;
};

}  // namespace

void CreateInviteUi(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post) {
  g_drawer = drawer;
  g_post = std::move(post);
  new InviteWatcher(drawer);  // retained by the drawer for the session
}

bool OpenInvitePicker() {
  if (!g_drawer || !g_post) return false;
  g_post([] {
    if (!ModalDialog::AnyOpen()) new InvitePicker(g_drawer);
  });
  return true;
}

}  // namespace tmnt
