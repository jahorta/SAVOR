#include "SettingsPane.h"

#include "../App.h"
#include "../../Components/ToastBus.h"
#include "imgui.h"

#include <filesystem>
#include <cstdio>
#include <string>

extern GuiApp g_app;

namespace {
    constexpr const char* SEC = "Settings";

    struct UiState {
        bool loaded = false;
        char db_root[512]{};
    };

    UiState& st() { static UiState s; return s; }

    void load_once() {
        if (st().loaded) return;
        auto cfg = g_app.GuiCfgGet(SEC, "db_root", "");
        if (cfg.empty()) cfg = g_app.GetDatabaseRoot();
        std::snprintf(st().db_root, sizeof(st().db_root), "%s", cfg.c_str());
        st().loaded = true;
    }
}

void SettingsPane::Draw() {
    load_once();

    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoMove);

    auto active_root = g_app.GetDatabaseRoot();
    ImGui::TextWrapped("Current database root: %s", active_root.c_str());
    ImGui::Spacing();

    ImGui::SetNextItemWidth(520);
    ImGui::InputTextWithHint("Database root", "Example: D:/SOASimData", st().db_root, sizeof(st().db_root));

    ImGui::TextWrapped("Changing this will stop DB services, move existing database files and object storage, then restart.");

    if (ImGui::Button("Apply and Move Data")) {
        const std::string target = st().db_root;
        if (target.empty()) {
            GuiToastBus::Post(GuiToastSeverity::Error, "Database root cannot be empty");
        } else {
            std::string err;
            if (g_app.ApplyDatabaseRootChange(target, err)) {
                g_app.GuiCfgSet(SEC, "db_root", target);
                GuiToastBus::Post(GuiToastSeverity::Info, "Database storage moved successfully");
            } else {
                GuiToastBus::Post(GuiToastSeverity::Error, err.empty() ? "Failed to relocate database" : err);
            }
        }
    }

    ImGui::End();
}
