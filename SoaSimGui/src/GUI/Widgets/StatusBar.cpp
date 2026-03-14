#include "StatusBar.h"
#include "imgui.h"
#include "Utils/Time.h"
#include "../App.h"   
#include "../../Components/ToastBus.h"

static void Pill(const char* text, ImU32 col_bg, ImU32 col_fg) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_Button, col_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, col_fg);
    ImGui::Button(text);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

extern GuiApp g_app;

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

    ImGui::SameLine();
    if (g_app.CoordinatorRunning()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Coordinator: %zu", g_app.CoordinatorActiveWorkers());
        Pill(buf, IM_COL32(64, 128, 255, 200), IM_COL32(255, 255, 255, 255));
    }
    else {
        Pill("Coordinator: Stopped", IM_COL32(128, 128, 128, 180), IM_COL32(255, 255, 255, 255));
    }

    auto toasts = GuiToastBus::SnapshotActive();
    if (!toasts.empty()) {
        ImGui::SameLine();
        for (auto& t : toasts) {
            ImU32 bg = 0, fg = IM_COL32(255, 255, 255, 255);
            switch (t.severity) {
            case GuiToastSeverity::Info:    bg = IM_COL32(64, 128, 255, 180); break;
            case GuiToastSeverity::Success: bg = IM_COL32(64, 160, 80, 180);  break;
            case GuiToastSeverity::Warn:    bg = IM_COL32(200, 160, 64, 220); break;
            case GuiToastSeverity::Error:   bg = IM_COL32(200, 80, 80, 220);  break;
            }
            ImGui::SameLine();
            char buf[512];
            if (t.count > 1) snprintf(buf, sizeof(buf), "%s ×%d", t.message.c_str(), t.count);
            else             snprintf(buf, sizeof(buf), "%s", t.message.c_str());
            Pill(buf, bg, fg);
            if (t.details && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.details->c_str());
        }
    }

    ImGui::End();

    ImGui::PopStyleVar();
}
