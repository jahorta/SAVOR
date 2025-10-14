#include "JobsPane.h"

#include "../Widgets/JobDetailDrawer.h"

#include "DB/Querying/JobListDTO.h"
#include "DB/Querying/Paging.h"
#include "DB/Querying/DataService.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/ProgramKindsRepo.h"

using namespace std::chrono;

namespace {
    struct State {
        JobsListScope scope{};
        int page_limit = 100;
        bool auto_refresh = true;
        int refresh_seconds = 3;

        std::optional<KeysetCursor> before{};
        std::optional<KeysetCursor> after{};

        Page<JobLite> page{};
        std::future<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> fut_kinds;
        bool kinds_in_flight = false;
        std::unordered_map<int, std::string> program_names;
        std::unordered_map<int64_t, std::string> progress_summary;

        int64_t selected_job_id = 0;

        steady_clock::time_point last_fetch{};
        bool fetch_in_flight = false;
        std::future<simcore::db::DbResult<Page<JobLite>>> fut_page;

        bool pending_apply_scroll = false;
        float pending_scroll_ratio = 0.0f;

        bool pk_loaded = false;

        std::optional<JobLite> open_job;
        bool drawer_open = false;
        int drawer_tab = 0;
    };

    static State& S() { static State s; return s; }

    static void kick_fetch() {
        auto& s = S();
        if (s.fetch_in_flight) return;
        s.fetch_in_flight = true;
        s.last_fetch = steady_clock::now();
        auto scope = s.scope;
        auto before = s.before;
        auto after = s.after;
        int limit = s.page_limit;
        s.fut_page = simcore::db::DataService::FetchJobsPageAsync(scope, before, after, limit);
    }

    static void maybe_refresh() {
        auto& s = S();
        if (!s.auto_refresh) return;
        if (s.before || s.after) return; // only refresh on newest page
        if (s.fetch_in_flight) return;
        if (duration_cast<seconds>(steady_clock::now() - s.last_fetch).count() >= s.refresh_seconds) {
            kick_fetch();
        }
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

    static void update_progress_async(const Page<JobLite>& page) {
        auto ids = std::vector<int64_t>{};
        ids.reserve(page.items.size());
        for (auto& r : page.items) ids.push_back(r.job_id);
        auto fut = simcore::db::DataService::BulkLatestProgressByJobsAsync(ids);
        std::thread([f = std::move(fut)]() mutable {
            auto r = f.get();
            if (!r.ok) return;
            auto& s = S();
            std::unordered_map<int64_t, std::string> m;
            for (auto& p : r.value) {
                if (p.payload) {
                    std::string v = *p.payload;
                    if (v.size() > 80) v.resize(80);
                    m[p.job_id] = std::move(v);
                }
            }
            s.progress_summary = std::move(m);
            }).detach();
    }

    static void consume_fetch_if_ready() {
        auto& s = S();
        if (!s.fetch_in_flight) return;
        using namespace std::chrono_literals;
        if (s.fut_page.valid() && s.fut_page.wait_for(0ms) == std::future_status::ready) {
            auto r = s.fut_page.get();
            s.fetch_in_flight = false;
            if (r.ok) {

                float y = ImGui::GetScrollY();
                float maxy = ImGui::GetScrollMaxY();
                s.pending_scroll_ratio = (maxy > 1.0f) ? (y / maxy) : 0.0f;
                s.pending_apply_scroll = true;

                s.page = std::move(r.value);
                update_progress_async(s.page);
                bool still = false;
                for (auto& it : s.page.items) if (it.job_id == s.selected_job_id) { still = true; break; }
                if (!still) s.selected_job_id = 0;
            }
            else {
                s.page.items.clear();
                s.page.next.reset();
                s.page.prev.reset();
            }
        }
    }

    static void apply_pending_scroll_if_any() {
        auto& s = S();
        if (!s.pending_apply_scroll) return;
        s.pending_apply_scroll = false;
        float maxy = ImGui::GetScrollMaxY();
        ImGui::SetScrollY(s.pending_scroll_ratio * maxy);
    }

    static const char* state_items[] = {
        "QUEUED","CLAIMED","RUNNING","SUCCEEDED","FAILED","CANCELED","SUPERSEDED","SUCCEEDED_WINNER","SUCCEEDED_DUPLICATE"
    };

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
            s.pk_loaded = true;
        }
    }
}

void JobsPane::OnActivated() {
    auto& s = S();
    s.program_names.clear();
    s.pk_loaded = false;
    kick_kinds_fetch();   // async via DataService; UI thread stays DB-free

    // Optional: also fetch newest jobs page immediately on activation
    s.before.reset(); s.after.reset();
    if (!s.fetch_in_flight) kick_fetch();
}

void JobsPane::Draw() {
    auto& s = S();

    ImGui::Begin("Jobs", nullptr, ImGuiWindowFlags_NoMove);

    ImGui::PushItemWidth(160);
    static int selected_kind = -1;
    static int selected_state = -1;
    static char jobset_buf[32] = {};
    static int page_sz = s.page_limit;
    static int refresh_sec = s.refresh_seconds;
    static bool auto_ref = s.auto_refresh;

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
    if (ImGui::BeginCombo("State", selected_state < 0 ? "All" : state_items[selected_state])) {
        if (ImGui::Selectable("All", selected_state < 0)) selected_state = -1;
        for (int i = 0; i < (int)(sizeof(state_items) / sizeof(state_items[0])); ++i) {
            bool sel = (selected_state == i);
            if (ImGui::Selectable(state_items[i], sel)) selected_state = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::InputTextWithHint("JobSet", "job_set_id", jobset_buf, IM_ARRAYSIZE(jobset_buf));
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        s.scope = {};
        if (selected_state >= 0) s.scope.states.push_back(state_items[selected_state]);
        if (selected_kind >= 0) s.scope.program_kind = selected_kind;
        if (jobset_buf[0]) s.scope.job_set_id = std::strtoll(jobset_buf, nullptr, 10);
        s.before.reset(); s.after.reset();
        s.page_limit = page_sz;
        s.auto_refresh = auto_ref;
        s.refresh_seconds = refresh_sec;
        kick_fetch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        selected_kind = -1; selected_state = -1; jobset_buf[0] = 0;
        s.scope = {};
        s.before.reset(); s.after.reset();
        s.page_limit = 100; page_sz = s.page_limit;
        s.auto_refresh = true; auto_ref = true;
        s.refresh_seconds = 3; refresh_sec = 3;
        kick_fetch();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto", &auto_ref);
    s.auto_refresh = auto_ref;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::SliderInt("Every (s)", &refresh_sec, 2, 5);
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

    ImGui::BeginChild("JobsTableRegion", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    consume_fetch_if_ready();
    consume_kinds_fetch_if_ready();
    apply_pending_scroll_if_any();
    maybe_refresh();

    if (ImGui::BeginTable("JobsTable", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("job_set_id");
        ImGui::TableSetupColumn("program_kind");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("queued_at");
        ImGui::TableSetupColumn("progress", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)s.page.items.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& r = s.page.items[i];
                ImGui::PushID((int)r.job_id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImVec2 row_top = ImGui::GetCursorScreenPos();

                
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(std::to_string(r.job_id).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::to_string(r.job_set_id).c_str());
                ImGui::TableSetColumnIndex(2);
                {
                    auto it = s.program_names.find(r.program_kind);
                    if (it != s.program_names.end()) ImGui::TextUnformatted(it->second.c_str());
                    else ImGui::Text("kind %d", r.program_kind);
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r.state.c_str());
                ImGui::TableSetColumnIndex(4);
                {
                    auto ts = fmt_time(r.queued_at);
                    ImGui::TextUnformatted(ts.c_str());
                }
                ImGui::TableSetColumnIndex(5);
                {
                    auto it = s.progress_summary.find(r.job_id);
                    if (it != s.progress_summary.end()) ImGui::TextUnformatted(it->second.c_str());
                    else ImGui::TextUnformatted("...");
                }
                bool sel = (s.selected_job_id == r.job_id);
                ImGui::SetCursorScreenPos(row_top);
                const float row_h = ImGui::GetTextLineHeightWithSpacing();
                if (ImGui::Selectable("##Row",
                    sel,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowDoubleClick |
                    ImGuiSelectableFlags_AllowItemOverlap,
                    ImVec2(0, row_h))) {
                    s.selected_job_id = r.job_id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        s.open_job = r;
                        s.drawer_open = true;
                        s.drawer_tab = 0;
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (s.drawer_open && s.open_job.has_value()) {
        if (!JobDetailsDrawer::Draw(*s.open_job, s.drawer_tab, s.program_names)) {
            s.drawer_open = false;
            s.open_job.reset();
        }
    }

    ImGui::End();
}
