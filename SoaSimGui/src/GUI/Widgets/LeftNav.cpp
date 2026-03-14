#include "LeftNav.h"
#include <string>

static GuiPane g_active = GuiPane::Jobs;
static bool g_debugger_hook_active = false;

GuiPane GuiLeftNav::GetActive() { return g_active; }
void GuiLeftNav::SetActive(GuiPane p) { g_active = p; }
void GuiLeftNav::SetDebuggerHookActive(bool active) { g_debugger_hook_active = active; }

void GuiLeftNav::Draw() {
    ImGui::Begin("Navigation", nullptr, ImGuiWindowFlags_NoMove);

    for (int i = 0; i < 12; ++i) {
        bool disabled = (i > 11);
        if (disabled) ImGui::BeginDisabled(true);
        bool sel = ((int)g_active == i);
        std::string label = items[i];
        if (i == (int)GuiPane::Debugger && g_debugger_hook_active) label = "• " + label;
        if (ImGui::Selectable(label.c_str(), sel)) g_active = (GuiPane)i;
        if (disabled) {
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coming soon");
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
}
