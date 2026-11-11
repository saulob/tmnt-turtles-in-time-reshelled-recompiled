// The embedded launcher's Ctrl+S path: final patched TRG source and its TMNT wiring.
// Run: tmnt_launcher_hotkey_tests

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

void TestCtrlSInFinalPatchedTrg() {
  std::printf("Ctrl+S is detected from the real left/right key state before text input\n");
  const std::string source = Read(std::string(TMNT_TRG_PATCHED_DIR) + "/src/launcher.cpp");
  const size_t hotkeys = source.find("void Launcher::HandleHotkeys() {");
  CHECK(hotkeys != std::string::npos);
  const std::string body = source.substr(hotkeys);

  const size_t capture = body.find("if (capturing_key() || ImGui::IsPopupOpen");
  const size_t ctrl = body.find(
      "const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);");
  const size_t pressed = body.find("if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {");
  const size_t save = body.find("Save();", pressed);
  const size_t text_guard = body.find("if (io.WantTextInput) return;");
  const size_t enter = body.find("ImGui::IsKeyPressed(ImGuiKey_Enter, false)");

  CHECK(capture != std::string::npos);
  CHECK(ctrl != std::string::npos);
  CHECK(pressed != std::string::npos);
  CHECK(save != std::string::npos);
  CHECK(text_guard != std::string::npos);
  CHECK(enter != std::string::npos);
  CHECK(capture < ctrl && ctrl < pressed && pressed < save && save < text_guard && text_guard < enter);
  CHECK(!Has(body, "io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S"));
  CHECK(Has(body, "ImGui::IsKeyPressed(ImGuiKey_S, false)"));
  CHECK(!Has(body, "ImGui::IsKeyPressed(ImGuiKey_S, true)"));
}

void TestSaveButtonUsesSameAction() {
  std::printf("the Save button and Ctrl+S call the same Launcher::Save\n");
  const std::string source = Read(std::string(TMNT_TRG_PATCHED_DIR) + "/src/launcher.cpp");
  CHECK(Has(source, "if (ImGui::Button(T(\"Save\"), ImVec2(bw, bh))) Save();"));
  CHECK(Has(source, "if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {"));
  CHECK(Has(source, "if (config_.on_save) ok = config_.on_save() && ok;"));
  CHECK(Has(source, "if (config_.on_save_result) config_.on_save_result(ok);"));
  CHECK(Has(source, "SetStatus(ok ? \"Settings saved.\" : \"Could not save the settings.\""));
}

void TestEnterAndControllerRemainGuarded() {
  std::printf("Enter stays behind the text-input guard and controller actions are unchanged\n");
  const std::string source = Read(std::string(TMNT_TRG_PATCHED_DIR) + "/src/launcher.cpp");
  const size_t hotkeys = source.find("void Launcher::HandleHotkeys() {");
  const std::string body = source.substr(hotkeys);
  CHECK(Has(body, "if (io.WantTextInput) return;"));
  CHECK(Has(body, "if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {"));
  CHECK(Has(body, "if (!io.NavVisible && !ImGui::IsAnyItemActive()) want_play_ = true;"));
  CHECK(Has(body, "else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {"));
  CHECK(body.find("if (io.WantTextInput) return;") < body.find("ImGuiKey_GamepadStart"));
}

void TestRuntimeWiringHasNoTemporaryDiagnostics() {
  std::printf("the embedded TMNT wiring keeps the Save path and no temporary diagnostics\n");
  const std::string launcher = Read(std::string(TMNT_SRC_DIR) + "/launcher.cpp");
  CHECK(Has(launcher, "c.settings = &settings_;"));
  CHECK(Has(launcher, "c.save_on_play = false;"));
  CHECK(!Has(launcher, "DEBUG_SAULO"));
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestCtrlSInFinalPatchedTrg();
  TestSaveButtonUsesSameAction();
  TestEnterAndControllerRemainGuarded();
  TestRuntimeWiringHasNoTemporaryDiagnostics();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
