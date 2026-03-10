#include "JobSetsPane.h"

#include "JobsPane.h"
#include "../Widgets/LeftNav.h"
#include "../../Components/ToastBus.h"

#include "imgui.h"
#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"

#include <chrono>
#include <ctime>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

namespace {
    struct State {
        JobSetsListScope scope{};
        int page_limit = 100;

        bool auto_refresh = true;
        int refresh_seconds = 2;

        std::optional<KeysetCursor> before{};
        std::optional<KeysetCursor> after{};

        Page<JobSetLite> page{};
        std::future<simcore::db::DbResult<Page<JobSetLite>>> fut_page;
        bool fetch_in_flight = false;

        std::future<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> fut_kinds;
        bool kinds_in_flight = false;
        std::unordered_map<int, std::string> program_names;

        int64_t pending_delete_job_set_id = 0;
        std::future<simcore::db::DbResult<void>> fut_delete;
        bool delete_in_flight = false;

        steady_clock::time_point last_fetch{};
    };

    static State& S() {
        static State s;
        return s;
    }

    static std::string fmt_time(int64_t epoch_sec) {
        std::time_t t = (std::time_t)epoch_sec;
        char buf[32]{ 0 };
#if defined(_WIN32)
        std::tm tm{};
        localtime_s(&tm, &t);
#else
        std::tm tm = *std::localtime(&t);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::string(buf);
    }

    static void kick_fetch() {
        auto& s = S();
        if (s.fetch_in_flight) return;
        s.fetch_in_flight = true;
        s.last_fetch = steady_clock::now();

        PagedQuery<> q{};
        q.before = s.before;
        q.after = s.after;
        q.limit = s.page_limit;
        s.fut_page = simcore::db::DataService::FetchJobSetsPage(s.scope, q);
    }

    static void maybe_refresh() {
        auto& s = S();
        if (!s.auto_refresh) return;
        if (s.before || s.after) return; // only newest page
        if (s.fetch_in_flight) return;
        if (duration_cast<seconds>(steady_clock::now() - s.last_fetch).count() >= s.refresh_seconds) {
            kick_fetch();
        }
    }

    static void consume_fetch_if_ready() {
        auto& s = S();
        if (!s.fetch_in_flight) return;
        using namespace std::chrono_literals;
        if (!s.fut_page.valid()) return;
        if (s.fut_page.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_page.get();
        s.fetch_in_flight = false;
        if (r.ok) {
            s.page = std::move(r.value);
        }
        else {
            s.page = {};
        }
    }

    static void kick_kinds_fetch() {
        auto& s = S();
        if (s.kinds_in_flight) return;
        s.kinds_in_flight = true;
        s.fut_kinds = simcore::db::DataService::ListProgramKindsAsync();
    }

    static void consume_kinds_fetch_if_ready() {
        auto& s = S();
        using namespace std::chrono_literals;
        if (!s.kinds_in_flight) return;
        if (!s.fut_kinds.valid()) return;
        if (s.fut_kinds.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_kinds.get();
        s.kinds_in_flight = false;
        if (r.ok) {
            s.program_names.clear();
            for (auto& kv : r.value) s.program_names[kv.id] = kv.name;
        }
    }

    static void consume_delete_if_ready() {
        auto& s = S();
        if (!s.delete_in_flight) return;
        using namespace std::chrono_literals;
        if (!s.fut_delete.valid()) return;
        if (s.fut_delete.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_delete.get();
        s.delete_in_flight = false;
        if (r.ok) {
            GuiToastBus::Success("Job set deleted");
            s.before.reset();
            s.after.reset();
            kick_fetch();
        }
        else {
            GuiToastBus::Error("Delete failed", r.error.message);
        }
    }
}

void JobSetsPane::OnActivated() {
    auto& s = S();
    s.before.reset();
    s.after.reset();
    s.program_names.clear();
    kick_kinds_fetch();
    if (!s.fetch_in_flight) kick_fetch();
}

void JobSetsPane::Draw() {
    auto& s = S();

    ImGui::Begin("Job Sets", nullptr, ImGuiWindowFlags_NoMove);

    static int selected_kind = -1;
    static int page_sz = s.page_limit;
    static bool auto_ref = s.auto_refresh;
    static int refresh_sec = s.refresh_seconds;

    ImGui::PushItemWidth(160);
    ImGui::TextUnformatted("Filter:");
    ImGui::SameLine();
    if (ImGui::BeginCombo("Kind", selected_kind < 0 ? "All" : s.program_names[selected_kind].c_str())) {
        if (ImGui::Selectable("All", selected_kind < 0)) selected_kind = -1;
        for (auto& kv : s.program_names) {
            bool sel = (selected_kind == kv.first);
            if (ImGui::Selectable(kv.second.c_str(), sel)) selected_kind = kv.first;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        s.scope = {};
        if (selected_kind >= 0) s.scope.program_kind = selected_kind;
        s.before.reset();
        s.after.reset();
        s.page_limit = page_sz;
        s.auto_refresh = auto_ref;
        s.refresh_seconds = refresh_sec;
        kick_fetch();
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        selected_kind = -1;
        s.scope = {};
        s.before.reset();
        s.after.reset();
        s.page_limit = 100;
        page_sz = s.page_limit;
        s.auto_refresh = true;
        auto_ref = true;
        s.refresh_seconds = 2;
        refresh_sec = 2;
        kick_fetch();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto", &auto_ref);
    s.auto_refresh = auto_ref;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::SliderInt("Every (s)", &refresh_sec, 1, 5);
    s.refresh_seconds = refresh_sec;

    ImGui::Separator();

    bool has_prev = s.page.prev.has_value();
    bool has_next = s.page.next.has_value();
    if (!has_prev) ImGui::BeginDisabled();
    if (ImGui::Button("Prev")) {
        s.after = s.page.prev;
        s.before.reset();
        kick_fetch();
    }
    if (!has_prev) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!has_next) ImGui::BeginDisabled();
    if (ImGui::Button("Next")) {
        s.before = s.page.next;
        s.after.reset();
        kick_fetch();
    }
    if (!has_next) ImGui::EndDisabled();

    ImGui::Separator();

    consume_fetch_if_ready();
    consume_kinds_fetch_if_ready();
    consume_delete_if_ready();
    maybe_refresh();

    if (ImGui::BeginTable("JobSetsTable", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("job_set_id");
        ImGui::TableSetupColumn("program_kind");
        ImGui::TableSetupColumn("purpose", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("created_at");
        ImGui::TableSetupColumn("progress", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("open_jobs");
        ImGui::TableHeadersRow();

        for (const auto& r : s.page.items) {
            ImGui::PushID((int)r.job_set_id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%lld", (long long)r.job_set_id);

            ImGui::TableSetColumnIndex(1);
            {
                auto it = s.program_names.find(r.program_kind);
                if (it != s.program_names.end()) ImGui::TextUnformatted(it->second.c_str());
                else ImGui::Text("kind %d", r.program_kind);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.purpose.empty() ? "(none)" : r.purpose.c_str());

            ImGui::TableSetColumnIndex(3);
            {
                auto ts = fmt_time(r.created_at);
                ImGui::TextUnformatted(ts.c_str());
            }

            ImGui::TableSetColumnIndex(4);
            {
                const float frac = (r.total_jobs > 0) ? (float)r.completed_jobs / (float)r.total_jobs : 0.0f;
                std::string overlay = std::to_string((long long)r.completed_jobs) + " / " + std::to_string((long long)r.total_jobs);
                ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay.c_str());
                if (r.expected_total.has_value()) {
                    ImGui::TextDisabled("planned: %lld", (long long)*r.expected_total);
                }
            }

            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Open Jobs")) {
                JobsPane::FocusJobSet(r.job_set_id);
                GuiLeftNav::SetActive(GuiPane::Jobs);
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(s.delete_in_flight);
            if (ImGui::SmallButton("Delete")) {
                s.pending_delete_job_set_id = r.job_set_id;
                ImGui::OpenPopup("Delete Job Set Confirm");
            }
            ImGui::EndDisabled();

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (ImGui::BeginPopupModal("Delete Job Set Confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Are you sure?");
        ImGui::TextWrapped("This will permanently delete this job set and all child job sets, jobs, and job history generated by them.");
        ImGui::Separator();
        ImGui::Text("job_set_id: %lld", (long long)s.pending_delete_job_set_id);

        ImGui::BeginDisabled(s.delete_in_flight || s.pending_delete_job_set_id <= 0);
        if (ImGui::Button("Yes, delete it")) {
            const int64_t id = s.pending_delete_job_set_id;
            s.delete_in_flight = true;
            s.fut_delete = simcore::db::DataService::DeleteJobSetAsync(id);
            s.pending_delete_job_set_id = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            s.pending_delete_job_set_id = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}
