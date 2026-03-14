#include "JobDetailDrawer.h"
#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"
#include "../App.h"
#include "../Popups/IniEditor.h"
#include "Utils/String.h"
#include <chrono>
#include <algorithm>
using namespace simcore::db;

namespace {
    struct TabState {
        bool payload_loaded = false;
        bool results_loaded = false;
        bool artifacts_loaded = false;

        std::optional<IniDoc> payload_ini;
        std::optional<IniDoc> results_ini;
        std::vector<ArtifactRefLite> artifacts;

        // Events (progress fallback)
        SnapshotMailbox<Page<JobEventLite>> events_mb;
        std::unique_ptr<DataService::PollHandle> events_handle;
        Page<JobEventLite> events_cache;

        // Decoded progress (optional)
        bool show_decoded = false;
        bool decoded_progress_loaded = false;
        std::optional<std::vector<std::string>> decoded_progress_lines;
        std::future<DbResult<std::string>> decoded_fut;

        bool requeue_opts_open = false;
        IniEditorModalState ini_editor;
        ImGuiID popup_viewport_id = 0;

        std::optional<int64_t> debug_request_id;
        std::optional<simcore::db::DebugSessionRow> debug_session;
        std::optional<simcore::WorkerCoordinator::DebugRuntimeSnapshot> debug_runtime;
        std::chrono::steady_clock::time_point next_debug_refresh{};

        void reset() {
            if (events_handle) { events_handle->stop(); events_handle.reset(); }
            // clear latest snapshot(s) without blocking
            while (events_mb.has_value()) events_mb.pop();
            events_cache = Page<JobEventLite>{};

            payload_loaded = false;
            results_loaded = false;
            artifacts_loaded = false;

            payload_ini.reset();
            results_ini.reset();
            artifacts.clear();

            decoded_progress_loaded = false;
            decoded_progress_lines.reset();

            // reset future to an empty state
            decoded_fut = std::future<DbResult<std::string>>{};
            debug_request_id.reset();
            debug_session.reset();
            debug_runtime.reset();
            next_debug_refresh = {};
        }
    };

    static TabState g;
    static std::chrono::steady_clock::time_point g_next_debug_slot_busy_toast = std::chrono::steady_clock::time_point::min();
}

static void DrawIniDoc(const IniDoc& doc) {
    ImGui::BeginChild("ini_root", ImVec2(0, 0), false);

    auto sections = doc.list_sections();
    if (sections.size() > 0) {
        ImGui::Columns(2, nullptr, true);
        ImGui::TextUnformatted("Section"); ImGui::NextColumn();
        ImGui::TextUnformatted("Key = Value"); ImGui::NextColumn();
        for (const auto& section_name : sections) {

            ImGui::Separator();
            ImGui::TextUnformatted(section_name.c_str()); ImGui::NextColumn();
            for (const auto& kv : doc.section_kv(section_name).kv) {
                ImGui::TextUnformatted((kv.first + " = " + kv.second).c_str());
            }
            ImGui::NextColumn();
        }
    }
    else {
        ImGui::Separator();
        ImGui::TextUnformatted("Empty Ini");
    }
    ImGui::EndChild();
}

bool JobDetailsDrawer::Draw(const JobLite& job, int& active_tab, std::unordered_map<int, std::string>& program_names) {
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 560.0f, 40.0f), ImGuiCond_FirstUseEver);
    bool open = true;

    if (!ImGui::Begin("Job Details", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return open;
    }

    const auto it = program_names.find(job.program_kind);
    const char* kind_name = (it != program_names.end()) ? it->second.c_str() : "<unknown>";
    ImGui::Text("Job: %lld  |  Set: %lld  |  ProgramKind: %s  |  State: %s",
        (long long)job.job_id, (long long)job.job_set_id, kind_name, job.state.c_str());

    bool requeue_blocked = (job.state == "QUEUED" || job.state == "CLAIMED" || job.state == "RUNNING" || job.state == "FAILED");
    ImGui::BeginDisabled(requeue_blocked);
    if (ImGui::Button("Requeue")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::RequeueJobAsync(job.job_id),
            [](const DbResult<void>& r) { if (r.ok) GuiToastBus::Success("Job requeued"); else GuiToastBus::Error("Requeue failed", r.error.message); },
            [](std::exception_ptr) { GuiToastBus::Error("Requeue failed", "exception"); }
        );
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && requeue_blocked) {
        if (job.state == "FAILED") ImGui::SetTooltip("FAILED jobs require Restart (resets attempts)");
        else ImGui::SetTooltip("Cannot requeue while job is QUEUED/CLAIMED/RUNNING");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(job.state != "FAILED");
    if (ImGui::Button("Restart")) {
        g.requeue_opts_open = true;
        ImGui::OpenPopup("Restart Failed Job");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::CancelJobAsync(job.job_id),
            [](const DbResult<void>& r) { if (r.ok) GuiToastBus::Warn("Job canceled"); else GuiToastBus::Error("Cancel failed", r.error.message); },
            [](std::exception_ptr) { GuiToastBus::Error("Cancel failed", "exception"); }
        );
    }

    ImGui::SameLine();
    const bool is_terminal = (job.state == "SUCCEEDED" || job.state == "FAILED" || job.state == "CANCELED" || job.state == "SUPERSEDED" || job.state == "SUCCEEDED_WINNER" || job.state == "SUCCEEDED_DUPLICATE");
    ImGui::BeginDisabled(!is_terminal);
    if (ImGui::Button("Start Visual Debug")) {
        auto sr = g_app.StartVisualDebug(job.job_id, "gui");
        if (sr.ok) {
            g.debug_request_id = sr.request_id;
            GuiToastBus::Info("Debug startup", "request " + std::to_string((long long)sr.request_id));
        }
        else {
            if (sr.error == "DebugSlotBusy") {
                const auto now = std::chrono::steady_clock::now();
                if (now >= g_next_debug_slot_busy_toast) {
                    GuiToastBus::Info("Debug Slot Busy", "Debugger is currently in use.");
                    g_next_debug_slot_busy_toast = now + std::chrono::seconds(2);
                }
            }
            else if (sr.error == "JobNotTerminal") {
                GuiToastBus::Warn("Job not terminal", "Only terminal jobs can be debugged.");
            }
            else {
                GuiToastBus::Error("Start Visual Debug failed", sr.error);
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !is_terminal) {
        ImGui::SetTooltip("Only terminal-state jobs are debug-eligible");
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop Debugging")) {
        if (g.debug_session.has_value()) {
            auto rr = g_app.StopVisualDebug(g.debug_session->id);
            if (rr.ok) GuiToastBus::Warn("Debug session stopped");
            else GuiToastBus::Error("Stop Debugging failed", rr.error.message);
        }
        else {
            GuiToastBus::Warn("No active debug session", "Nothing to stop");
        }
    }

    ImGui::SameLine();
    const bool can_cancel_start = g.debug_session.has_value() && (g.debug_session->state == "starting" || g.debug_session->state == "launching_worker");
    ImGui::BeginDisabled(!can_cancel_start);
    if (ImGui::Button("Cancel Debug Start")) {
        auto rr = g_app.CancelVisualDebugStart(g.debug_session->id);
        if (rr.ok) {
            GuiToastBus::Warn("Debug startup canceled");
            g.debug_session.reset();
            g.debug_runtime.reset();
            g.debug_request_id.reset();
        }
        else if (rr.error.message == "TooLate") {
            GuiToastBus::Info("Too late to cancel", "Use Stop Debugging once attached.");
        }
        else {
            GuiToastBus::Error("Cancel Debug Start failed", rr.error.message);
        }
    }
    ImGui::EndDisabled();
    static int bump_delta = 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("Bump Delta", &bump_delta);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::BumpPriorityAsync(job.job_id, bump_delta),
            [](const DbResult<void>& r) { if (r.ok) GuiToastBus::Info("Priority bumped"); else GuiToastBus::Error("Bump failed", r.error.message); },
            [](std::exception_ptr) { GuiToastBus::Error("Bump failed", "exception"); }
        );
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) g.reset();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 c = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();

    // Restart options modal (FAILED only)
    if (ImGui::BeginPopupModal("Restart Failed Job", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This job failed. Restart will reset attempts to 0 and queue it again.");
        ImGui::Separator();
        if (ImGui::Button("Edit INI...")) {
            std::string ini_text;
            if (g.payload_loaded && g.payload_ini.has_value()) {
                ini_text = g.payload_ini->to_string_preserve_order();
            }
            else {
                auto r = DataService::FetchJobVmKvIniAsync(job.job_id).get();
                if (r.ok) ini_text = r.value.to_string_preserve_order(); else ini_text.clear();
            }
            Widgets::OpenIniEditor(g.ini_editor, std::move(ini_text));
            ImGui::CloseCurrentPopup();
            g.requeue_opts_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            FutureQueue::Enqueue<DbResult<void>>(
                DataService::RestartFailedJobAsync(job.job_id),
                [](const DbResult<void>& r) { if (r.ok) GuiToastBus::Success("Job restarted"); else GuiToastBus::Error("Restart failed", r.error.message); },
                [](std::exception_ptr) { GuiToastBus::Error("Restart failed", "exception"); }
            );
            ImGui::CloseCurrentPopup();
            g.requeue_opts_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            g.requeue_opts_open = false;
        }
        ImGui::EndPopup();
    }

    // INI Editor modal (standalone)
    if (Widgets::DrawIniEditor(g.ini_editor, "Edit Job INI", "Edit Job INI")) {
        if (g.ini_editor.ok_clicked && !g.ini_editor.in_flight) {
            g.ini_editor.in_flight = true;
            g.ini_editor.error_msg.reset();
            auto text = g.ini_editor.buffer;
            FutureQueue::Enqueue<DbResult<void>>(
                DataService::SetJobVmKvAsync(job.job_id, std::make_optional<std::string>(std::move(text))),
                [&](const DbResult<void>& r) {
                    if (!r.ok) {
                        g.ini_editor.in_flight = false;
                        g.ini_editor.error_msg = r.error.message;
                        GuiToastBus::Error("INI update failed", r.error.message);
                        return;
                    }
                    FutureQueue::Enqueue<DbResult<void>>(
                        DataService::RestartFailedJobAsync(job.job_id),
                        [&](const DbResult<void>& rr) {
                            g.ini_editor.in_flight = false;
                            if (rr.ok) {
                                GuiToastBus::Success("Updated INI & restarted");
                                g.ini_editor.dismiss_after_success = true;
                            }
                            else {
                                g.ini_editor.error_msg = rr.error.message;
                                GuiToastBus::Error("Restart failed", rr.error.message);
                            }
                        },
                        [&](std::exception_ptr) {
                            g.ini_editor.in_flight = false;
                            g.ini_editor.error_msg = std::string("exception");
                            GuiToastBus::Error("Restart failed", "exception");
                        }
                    );
                },
                [&](std::exception_ptr) {
                    g.ini_editor.in_flight = false;
                    g.ini_editor.error_msg = std::string("exception");
                    GuiToastBus::Error("INI update failed", "exception");
                }
            );
        }
    }
    ImGui::Separator();

    if (std::chrono::steady_clock::now() >= g.next_debug_refresh) {
        g.next_debug_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        auto ar = g_app.GetActiveVisualDebugSessionForJob(job.job_id);
        if (ar.ok) {
            g.debug_session = ar.value;
            if (g.debug_session.has_value()) g.debug_request_id = g.debug_session->id;
            if (g.debug_session.has_value()) {
                auto sr = g_app.GetVisualDebugRuntimeSnapshot(g.debug_session->id);
                if (sr.ok) g.debug_runtime = sr.value;
            }
        }
    }

    if (ImGui::BeginTabBar("job_tabs")) {
        if (ImGui::BeginTabItem("Overview")) {
            ImGui::Text("Priority: %d", job.priority);
            ImGui::Text("Queued at: %lld", (long long)job.queued_at);
            if (g.debug_request_id.has_value()) {
                ImGui::Text("Debug Request: %lld", (long long)*g.debug_request_id);
            }
            ImGui::EndTabItem();
            active_tab = 0;
        }

        if (ImGui::BeginTabItem("Visual Debugger")) {
            if (!g_app.CoordinatorRunning()) {
                ImGui::TextUnformatted("Coordinator must be running to use Visual Debugger.");
            }
            else if (!g.debug_session.has_value()) {
                ImGui::TextUnformatted("No active debug session for this job.");
            }
            else {
                const auto& ds = *g.debug_session;
                ImGui::Text("Session %lld | state: %s", (long long)ds.id, ds.state.c_str());
                ImGui::Text("VM endpoint: %s", ds.vm_endpoint.value_or("<pending>").c_str());
                ImGui::Text("Dolphin endpoint: %s", ds.dolphin_endpoint.value_or("<pending>").c_str());
                ImGui::Separator();
                ImGui::TextUnformatted("Execution Controls");
                if (ImGui::Button("Step VM Instruction")) {
                    auto rr = g_app.StepVisualDebugVmInstruction(ds.id);
                    if (!rr.ok) GuiToastBus::Error("Step VM failed", rr.error.message);
                }
                ImGui::SameLine();
                if (ImGui::Button("Step Frame")) {
                    auto rr = g_app.StepVisualDebugFrame(ds.id);
                    if (!rr.ok) GuiToastBus::Error("Step Frame failed", rr.error.message);
                }
                ImGui::SameLine();
                if (ImGui::Button("Run To BP")) {
                    auto rr = g_app.RunVisualDebugToBreakpoint(ds.id);
                    if (!rr.ok) GuiToastBus::Error("Run To BP failed", rr.error.message);
                }
                ImGui::SameLine();
                if (ImGui::Button("Pause")) {
                    auto rr = g_app.PauseVisualDebug(ds.id);
                    if (!rr.ok) GuiToastBus::Error("Pause failed", rr.error.message);
                }

                if (g.debug_runtime.has_value()) {
                    const auto& rt = *g.debug_runtime;
                    ImGui::Separator();
                    ImGui::Text("VM: %s | EMU: %s | MODE: %s", rt.vm_state.c_str(), rt.emu_state.c_str(), rt.ux_mode.c_str());
                    ImGui::Text("Script Position: %s @ 0x%08X", rt.script_name.c_str(), rt.script_pc);
                    ImGui::Text("Current Input: %s", rt.current_input.c_str());
                    ImGui::Text("Frame: %lld | seq: %lld | reason: %s", (long long)rt.frame_index, (long long)rt.sequence, rt.break_reason.c_str());

                    ImGui::SeparatorText("Breakpoints");
                    if (ImGui::BeginTable("dbg_bp_tbl", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                        ImGui::TableSetupColumn("Script Step");
                        ImGui::TableHeadersRow();
                        for (int step = 0; step < 8; ++step) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            const bool enabled = std::find(rt.breakpoints.begin(), rt.breakpoints.end(), step) != rt.breakpoints.end();
                            ImGui::PushID(step);
                            if (ImGui::Selectable(enabled ? "●" : " ", false, ImGuiSelectableFlags_SpanAllColumns)) {
                                (void)g_app.ToggleVisualDebugBreakpoint(ds.id, step, !enabled);
                            }
                            ImGui::PopID();
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("Step %d", step);
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Events")) {
            if (!g.events_handle) {
                JobEventsListScope scope{};
                scope.job_id = job.job_id;
                g.events_handle = DataService::StartJobEventsPolling(scope, std::chrono::milliseconds(700), 128, g.events_mb);
            }
            // drain mailbox
            while (g.events_mb.has_value()) {
                g.events_cache = g.events_mb.peek();
                g.events_mb.pop();
            }
            ImGui::BeginChild("events_list", ImVec2(0, 0), true);
            for (const auto& e : g.events_cache.items) {
                ImGui::Text("%lld  %s",
                    (long long)e.ts,
                    e.event_kind.c_str());
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
            active_tab = 1;
        }

        if (ImGui::BeginTabItem("Payload")) {
            if (!g.payload_loaded) {
                auto fut = DataService::FetchJobVmKvIniAsync(job.job_id);
                auto r = fut.get();
                if (r.ok) { g.payload_ini = std::move(r.value); g.payload_loaded = true; }
            }
            if (g.payload_ini) DrawIniDoc(*g.payload_ini);
            ImGui::EndTabItem();
            active_tab = 2;
        }

        if (ImGui::BeginTabItem("Artifacts")) {
            if (ImGui::Button("Update Artifacts"))
                g.artifacts_loaded = false;
            if (!g.artifacts_loaded) {
                auto fut = DataService::FetchJobArtifactRefsAsync(job.job_id);
                auto r = fut.get();
                if (r.ok) { g.artifacts = std::move(r.value); g.artifacts_loaded = true; }
            }
            if (ImGui::BeginTable("art_tbl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Artifact ID");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("File");
                ImGui::TableHeadersRow();
                for (auto& a : g.artifacts) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(a.role.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", (long long)a.artifact_id);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)a.size_bytes);
                    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(a.filename.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
            active_tab = 3;
        }

        if (ImGui::BeginTabItem("Progress")) {
            if (!g.decoded_progress_loaded) {
                g.decoded_fut = DataService::FetchDecodedProgressAsync(job.job_id);
                auto r = g.decoded_fut.get();
                if (r.ok) { 
                    
                    g.decoded_progress_lines = string::splitStringByNewline(r.value); 
                    g.decoded_progress_loaded = true; }
            }

            ImGui::BeginChild("ProgressText");
            if (!g.decoded_progress_lines.has_value()) ImGui::Text("No progress...");
            else {
                for (auto l : *g.decoded_progress_lines) ImGui::Text(l.c_str());
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
            active_tab = 4;
        }

        if (ImGui::BeginTabItem("Results")) {
            if (!g.results_loaded) {
                auto fut = DataService::FetchJobResultsIniAsync(job.job_id);
                auto r = fut.get();
                if (r.ok) { g.results_ini = std::move(r.value); g.results_loaded = true; }
            }
            if (g.results_ini) DrawIniDoc(*g.results_ini);
            ImGui::EndTabItem();
            active_tab = 5;
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    if (!open) {
        if (g.events_handle) { g.events_handle->stop(); g.events_handle.reset(); }
        g.reset();
    }
    return open;
}
