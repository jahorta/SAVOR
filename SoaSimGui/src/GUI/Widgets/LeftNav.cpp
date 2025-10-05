#include "LeftNav.h"

static GuiPane g_active = GuiPane::Jobs;

GuiPane GuiLeftNav::GetActive() { return g_active; }
void GuiLeftNav::SetActive(GuiPane p) { g_active = p; }

void GuiLeftNav::Draw() {
    ImGui::Begin("Navigation", nullptr, ImGuiWindowFlags_NoMove);
    const char* items[] = {
        "Job Sets","Jobs","Workers","Artifacts","Programs","Triggers","Explorer Runs","Seed Probe","TAS Movies"
    };
    for (int i = 0; i < 9; ++i) {
        bool disabled = (i > 1); // only Jobs enabled for now
        if (disabled) ImGui::BeginDisabled(true);
        bool sel = ((int)g_active == i);
        if (ImGui::Selectable(items[i], sel)) g_active = (GuiPane)i;
        if (disabled) {
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coming soon");
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
}
