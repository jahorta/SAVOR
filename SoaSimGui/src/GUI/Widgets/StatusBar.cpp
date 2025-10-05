#include "StatusBar.h"
#include "imgui.h"
#include "Utils/Time.h"

static void Pill(const char* text, ImU32 col_bg, ImU32 col_fg) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_Button, col_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, col_fg);
    ImGui::Button(text);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GuiStatusBar::Draw(const GuiStatusModel& model) {
    auto s = model.get();

    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, 2.0f));

    ImGui::Begin("Status", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);

    if (s.connected) Pill("Connected", IM_COL32(30, 180, 60, 255), IM_COL32(255, 255, 255, 255));
    else {
        Pill("Disconnected", IM_COL32(200, 60, 60, 255), IM_COL32(255, 255, 255, 255));
        if (!s.last_error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.last_error.c_str());
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::Text("env: %s", s.env_label.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (s.last_heartbeat_tp.time_since_epoch().count() != 0) {
        ImGui::Text("Last refresh: %s", simcore::time_util::steady_to_cstr(s.last_heartbeat_tp));
    }
    else {
        ImGui::Text("Last refresh: --");
    }
    ImGui::End();

    ImGui::PopStyleVar();
}
