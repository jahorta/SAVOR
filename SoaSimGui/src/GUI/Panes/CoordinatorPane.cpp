#include "CoordinatorPane.h"

#define NOMINMAX
#include "../App.h"           // GuiApp
#include "../../Components/ToastBus.h"
#include "imgui.h"
#include "../GuiCommon.h"

// Access the app-owned services
extern GuiApp g_app;

namespace {
    constexpr const char* SEC = "Coordinator";

    // UI-only inputs when coordinator is stopped
    struct UiInputs {
        bool loaded = false;
        int  target_workers = 1;
        int  event_buf_cap = 64;
        char iso_path[512]{};
        char dolphin_base_dir[512]{};
        bool start_paused{ true };
    };
    UiInputs& instance() { static UiInputs s; return s; }

    static void load_from_ini_once() {
        auto& in = instance();
        if (in.loaded) return;

        // Strings
        auto ip = g_app.GuiCfgGet(SEC, "iso_path", "");
        auto db = g_app.GuiCfgGet(SEC, "dolphin_base", "");
        std::snprintf(in.iso_path, sizeof(in.iso_path), "%s", ip.c_str());
        std::snprintf(in.dolphin_base_dir, sizeof(in.dolphin_base_dir), "%s", db.c_str());

        // Ints
        int tw = g_app.GuiCfgGetInt(SEC, "target_workers", 1);
        if (tw < 1) tw = 1;
        in.target_workers = tw;
        g_app.SetCoordinatorTargetWorkers((size_t)tw); // seed the live/prestart target

        in.event_buf_cap = g_app.GuiCfgGetInt(SEC, "event_ring", 64);
        if (in.event_buf_cap < 8) in.event_buf_cap = 8;

        in.loaded = true;
    }
}

void CoordinatorPane::Draw() {
    ImGui::Begin("Workers", nullptr, ImGuiWindowFlags_NoMove);

    load_from_ini_once();

    const bool running = g_app.CoordinatorRunning();

    // Start (enabled only when NOT running)
    ImGui::BeginDisabled(running);
    bool start_clicked = ImGui::Button((const char*)u8"\u25B6");
    if (running && ImGui::IsItemHovered()) ImGui::SetTooltip("Start Coordinator");
    if (start_clicked) {
        WorkerCoordinatorConfig cfg{};
        cfg.max_concurrent_processes = (size_t)std::max(64, instance().target_workers);
        cfg.desired_workers = instance().target_workers;
        cfg.iso_path = instance().iso_path;
        cfg.dolphin_base_dir = instance().dolphin_base_dir;

        if (cfg.iso_path.empty()) GuiToastBus::Post(GuiToastSeverity::Error, "No Iso Path Set");
        if (cfg.dolphin_base_dir.empty()) GuiToastBus::Post(GuiToastSeverity::Error, "No Dolphin Base Dir Set");

        bool ready = (!cfg.iso_path.empty() && !cfg.dolphin_base_dir.empty());

        if (ready)
        {
            cfg.start_to_paused = instance().start_paused;
            g_app.StartCoordinator(cfg);
            g_app.SetCoordinatorEventBufferCapacity((size_t)std::max(8, instance().event_buf_cap));
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- Controls row: always visible; disabled state depends on running ---
    // Stop + Pause (enabled only when running)
    ImGui::BeginDisabled(!running);

    bool paused_now = g_app.CoordinatorPaused();
    bool pause_clicked = ImGui::Button((const char*)u8"\u23F8");
    if (paused_now) {
        ImVec2 pmin = ImGui::GetItemRectMin();
        ImVec2 pmax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(pmin, pmax, IM_COL32(220, 64, 64, 255), ImGui::GetStyle().FrameRounding, 0, 2.0f);
    }
    if (!running && ImGui::IsItemHovered()) ImGui::SetTooltip("Pause Coordinator");
    if (pause_clicked) g_app.SetCoordinatorPaused(!paused_now);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    bool stop_clicked = ImGui::Button((const char*)u8"\u25A0");
    if (!running && ImGui::IsItemHovered()) ImGui::SetTooltip("Stop Coordinator");
    if (stop_clicked) g_app.StopCoordinator();
    ImGui::EndDisabled();

    int target_now = (int)g_app.CoordinatorTargetWorkers();
    if (target_now < 1) target_now = 1; // clamp display if unset
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Target", &target_now, 1)) {
        target_now = std::max(target_now, 1);
        g_app.SetCoordinatorTargetWorkers((size_t)target_now);
        g_app.GuiCfgSetInt(SEC, "target_workers", target_now);
        instance().target_workers = target_now;
    }

    ImGui::SameLine();
    if (running) {
        ImGui::Text("Active: %zu", g_app.CoordinatorActiveWorkers());
    }
    else {
        ImGui::TextDisabled("Active: --");
    }

    ImGui::Separator();

    // Header controls
    ImGui::BeginDisabled(running);

    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("ISO", "Path to SkiesOfArcadia iso", instance().iso_path, sizeof(instance().iso_path)))
        g_app.GuiCfgSet(SEC, "iso_path", instance().iso_path);

    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("Dolphin base", "Path to DolphinQt base directory with portable.txt", instance().dolphin_base_dir, sizeof(instance().dolphin_base_dir)))
        g_app.GuiCfgSet(SEC, "dolphin_base", instance().dolphin_base_dir);

    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Event ring", &instance().event_buf_cap, 8)) {
        if (instance().event_buf_cap < 8) instance().event_buf_cap = 8;
        g_app.GuiCfgSetInt(SEC, "event_ring", instance().event_buf_cap);
    }

    ImGui::EndDisabled();

    ImGui::Separator();

    // Live worker table (snapshot)
    if (running) {
        auto rows = g_app.CoordinatorSnapshot();
        if (ImGui::BeginTable("workers_tbl", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Job");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Lease");
            ImGui::TableSetupColumn("Attempts");
            ImGui::TableSetupColumn("Last HB");
            ImGui::TableSetupColumn("DB OK");
            ImGui::TableSetupColumn("Err");
            ImGui::TableHeadersRow();
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%lld", (long long)r.worker_id);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.pid);
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(soasim::ui::WorkerStateLabel(r.state));
                ImGui::TableSetColumnIndex(3); ImGui::Text("%lld", (long long)(r.job_id ? *r.job_id : 0));
                const std::string kind_label = soasim::ui::WorkerProgramKindLabel(r.program_kind);
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(kind_label.c_str());
                ImGui::TableSetColumnIndex(5); ImGui::Text("%lld", (long long)(r.lease_expires_at ? *r.lease_expires_at : 0));
                ImGui::TableSetColumnIndex(6); ImGui::Text("%d / %d", r.attempts, r.max_attempts);
                ImGui::TableSetColumnIndex(7); ImGui::Text("%lld", (long long)r.last_heartbeat_mono_ns);
                ImGui::TableSetColumnIndex(8); ImGui::Text("%lld", (long long)r.last_successful_db_call_mono_ns);
                ImGui::TableSetColumnIndex(9); ImGui::TextUnformatted(r.last_error.c_str());
            }
            ImGui::EndTable();
        }
    }
    else {
        ImGui::TextUnformatted("Coordinator is stopped.");
    }

    ImGui::End();
}
