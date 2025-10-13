#include "JobDetailDrawer.h"
#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"
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
        std::optional<IniDoc> decoded_progress_ini;
        std::future<DbResult<IniDoc>> decoded_fut;

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
            decoded_progress_ini.reset();

            // reset future to an empty state
            decoded_fut = std::future<DbResult<IniDoc>>{};
        }
    };

    static TabState g;
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

    if (ImGui::Button("Requeue")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::RequeueJobAsync(job.job_id),
            // on_success
            [](const DbResult<void>& r) {
                if (r.ok) GuiToastBus::Success("Job requeued");
                else GuiToastBus::Error("Requeue failed", r.error.message);
            },
            // on_error
            [](std::exception_ptr ep) {
                GuiToastBus::Error("Requeue failed", "exception");
            });
        
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::CancelJobAsync(job.job_id),
            // on_success
            [](const DbResult<void>& r) {
                if (r.ok) GuiToastBus::Warn("Job canceled");
                else GuiToastBus::Error("Cancel failed", r.error.message);
            },
            // on_error
            [](std::exception_ptr ep) {
                GuiToastBus::Error("Cancel failed", "exception");
            });
    }
    static int bump_delta = 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("Bump Delta", &bump_delta);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        FutureQueue::Enqueue<DbResult<void>>(
            DataService::BumpPriorityAsync(job.job_id, bump_delta),
            // on_success
            [](const DbResult<void>& r) {
                if (r.ok) GuiToastBus::Info("Priority bumped"); 
                else GuiToastBus::Error("Bump failed", r.error.message);
            },
            // on_error
            [](std::exception_ptr ep) {
                GuiToastBus::Error("Bump failed", "exception");
            });
        
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("job_tabs")) {
        if (ImGui::BeginTabItem("Overview")) {
            ImGui::Text("Priority: %d", job.priority);
            ImGui::Text("Queued at: %lld", (long long)job.queued_at);
            ImGui::EndTabItem();
            active_tab = 0;
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
                g.decoded_fut = DataService::FetchDecodedProgressIniAsync(job.job_id);
                auto r = g.decoded_fut.get();
                if (r.ok) { g.decoded_progress_ini = std::move(r.value); g.decoded_progress_loaded = true; }
            }
            if (g.decoded_progress_ini) DrawIniDoc(*g.decoded_progress_ini);
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
