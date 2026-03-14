#include "DebuggerPane.h"

#include "../App.h"
#include "../Widgets/LeftNav.h"
#include "../Components/ToastBus.h"
#include "imgui.h"
#include <string>

namespace {
    int64_t g_selected_session_id = 0;
}

void DebuggerPane::OnActivated() {}

void DebuggerPane::Draw() {
    ImGui::Begin("Debugger", nullptr, ImGuiWindowFlags_NoMove);

    auto lr = g_app.ListRecentVisualDebugSessions(100);
    if (!lr.ok) {
        ImGui::Text("Failed to query debug sessions: %s", lr.error.message.c_str());
        ImGui::End();
        return;
    }

	bool has_active = false;
	for (const auto& s : lr.value) {
		if (simcore::db::DebugSessionsRepo::IsActiveState(s.state)) {
			has_active = true;
			break;
		}
	}
    GuiLeftNav::SetDebuggerHookActive(has_active);

    if (ImGui::BeginTable("dbg_sessions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Job");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Started By");
        ImGui::TableHeadersRow();
        for (const auto& s : lr.value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool sel = g_selected_session_id == s.id;
            if (ImGui::Selectable(std::to_string((long long)s.id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) g_selected_session_id = s.id;
            ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", (long long)s.job_id);
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(s.state.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(s.started_by.value_or("<unknown>").c_str());
        }
        ImGui::EndTable();
    }

    if (g_selected_session_id > 0) {
        ImGui::Separator();
        auto dr = g_app.GetVisualDebugRuntimeSnapshot(g_selected_session_id);
        if (dr.ok) {
            const auto& rt = dr.value;
            ImGui::Text("Session %lld / Job %lld", (long long)rt.session_id, (long long)rt.job_id);
            ImGui::Text("VM: %s | EMU: %s | MODE: %s", rt.vm_state.c_str(), rt.emu_state.c_str(), rt.ux_mode.c_str());
            ImGui::Text("Script: %s @ 0x%08X", rt.script_name.c_str(), rt.script_pc);
            ImGui::Text("Input: %s", rt.current_input.c_str());
            ImGui::Text("Frame: %lld | Seq: %lld", (long long)rt.frame_index, (long long)rt.sequence);

            if (ImGui::Button("Step VM")) (void)g_app.StepVisualDebugVmInstruction(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Step Frame")) (void)g_app.StepVisualDebugFrame(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Run To BP")) (void)g_app.RunVisualDebugToBreakpoint(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Pause")) (void)g_app.PauseVisualDebug(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                auto rr = g_app.StopVisualDebug(g_selected_session_id);
                if (!rr.ok) GuiToastBus::Error("Stop Debugging failed", rr.error.message);
                else GuiToastBus::Warn("Debug session stopped");
            }
        }
        else {
            ImGui::TextUnformatted("No runtime snapshot (session may not be active).");
        }
    }

    ImGui::End();
}
