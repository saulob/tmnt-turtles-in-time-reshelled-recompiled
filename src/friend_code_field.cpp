#include "friend_code_field.h"

#include <cctype>

#include <imgui.h>

#include "online/protocol.h"

namespace tmnt {
namespace {

constexpr size_t kMaxShownPaste = 32;

bool CtrlDown() {
  return ImGui::GetIO().KeyCtrl || ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
}

}  // namespace

std::string PastedFriendCode(std::string_view clipboard) {
  if (std::string code = online::NormalizeFriendCode(clipboard); !code.empty()) return code;
  std::string shown;
  for (const char c : clipboard) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= ' ' || u >= 0x7f) continue;
    if (shown.size() == kMaxShownPaste) break;
    shown += static_cast<char>(std::toupper(u));
  }
  return shown;
}

void FriendCodeField::ReadPaste(const FieldLog& log) {
  const ClipboardText clipboard = read_ ? read_(log) : ClipboardText{};
  if (clipboard.kind == ClipboardText::Kind::kEmpty) return;
  if (clipboard.kind == ClipboardText::Kind::kError) {
    log("online friend paste failed step=" + clipboard.text);
    return;
  }
  paste_ = PastedFriendCode(clipboard.text);
  has_paste_ = !paste_.empty();
}

int FriendCodeField::ApplyPaste(ImGuiInputTextCallbackData* data) {
  auto* field = static_cast<FriendCodeField*>(data->UserData);
  if (!field->has_paste_) return 0;
  field->has_paste_ = false;
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, field->paste_.c_str());
  data->SelectionStart = data->SelectionEnd = data->CursorPos;
  return 0;
}

void FriendCodeField::Draw(char* buffer, size_t size, const FieldLog& log) {
  const FieldLog quiet = [](const std::string&) {};
  const FieldLog& out = log ? log : quiet;
  if (was_active_ && CtrlDown() && ImGui::IsKeyPressed(ImGuiKey_V, false)) ReadPaste(out);

  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##add_code", "XXXX-XXXX", buffer, size,
                           ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CallbackAlways, ApplyPaste, this);
  const bool active = ImGui::IsItemActive();
  if (!active) has_paste_ = false;
  was_active_ = active;
}

bool CopyFriendCode(const std::string& code, const ClipboardWriter& write, const FieldLog& log) {
  const bool copied = write && write(code, log);
  if (!copied) log("online friend copy failed");
  return copied;
}

}  // namespace tmnt
