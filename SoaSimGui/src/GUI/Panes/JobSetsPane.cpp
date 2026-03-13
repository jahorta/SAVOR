#include "JobSetsPane.h"

#include "JobsPane.h"
#include "../Widgets/LeftNav.h"
#include "../../Components/ToastBus.h"

#include "imgui.h"
#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <functional>
#include <optional>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::chrono;

namespace {
    static constexpr const char* kDeleteJobSetPopupId = "Delete Job Set Confirm##JobSetsPane";

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

        std::future<simcore::db::DbResult<std::vector<JobSetLite>>> fut_family;
        bool family_in_flight = false;
        std::vector<JobSetLite> family_items;

        std::future<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> fut_kinds;
        bool kinds_in_flight = false;
        std::unordered_map<int, std::string> program_names;

        std::unordered_set<int64_t> expanded_job_set_ids;

        int64_t pending_delete_job_set_id = 0;
        bool request_delete_modal_open = false;
        std::future<simcore::db::DbResult<void>> fut_delete;
        bool delete_in_flight = false;

        std::future<simcore::db::DbResult<simcore::db::JobSetPriorityBoostResult>> fut_boost;
        bool boost_in_flight = false;

        std::future<simcore::db::DbResult<simcore::db::JobSetCancelQueuedResult>> fut_cancel_tree;
        bool cancel_tree_in_flight = false;

        steady_clock::time_point last_fetch{};
    };

    static State& S() {
        static State s;
        return s;
    }

    static const char* state_filter_label(const std::optional<JobSetStateFilter>& f) {
        if (!f.has_value()) return "All";
        switch (*f) {
        case JobSetStateFilter::Completed: return "Completed";
        case JobSetStateFilter::Incomplete: return "Incomplete";
        case JobSetStateFilter::HasFailures: return "Has failures";
        }
        return "All";
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

    static void draw_segmented_progress(int64_t succeeded, int64_t failed, int64_t canceled, int64_t completed, int64_t total) {
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(width, height);
        ImGui::InvisibleButton("##seg_progress", sz);

        auto* dl = ImGui::GetWindowDrawList();
        const ImU32 col_bg = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
        const ImU32 col_success = ImGui::GetColorU32(ImVec4(0.12f, 0.45f, 0.16f, 1.0f));
        const ImU32 col_failed = ImGui::GetColorU32(ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        const ImU32 col_canceled = ImGui::GetColorU32(ImVec4(0.35f, 0.22f, 0.12f, 1.0f));
        const ImU32 col_remain = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.30f, 1.0f));

        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), col_bg, 3.0f);

        if (total <= 0) return;

        const int64_t success_clamped = (std::max)(int64_t(0), (std::min)(succeeded, total));
        const int64_t failed_clamped = (std::max)(int64_t(0), (std::min)(failed, total - success_clamped));
        const int64_t canceled_clamped = (std::max)(int64_t(0), (std::min)(canceled, total - success_clamped - failed_clamped));
        const int64_t remain = (std::max)(int64_t(0), total - success_clamped - failed_clamped - canceled_clamped);

        const float success_w = sz.x * (float)success_clamped / (float)total;
        const float failed_w = sz.x * (float)failed_clamped / (float)total;
        const float canceled_w = sz.x * (float)canceled_clamped / (float)total;
        const float remain_w = sz.x * (float)remain / (float)total;

        float x0 = p.x;
        if (success_w > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + success_w, p.y + sz.y), col_success);
            x0 += success_w;
        }
        if (failed_w > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + failed_w, p.y + sz.y), col_failed, 0.0f);
            x0 += failed_w;
        }
        if (canceled_w > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + canceled_w, p.y + sz.y), col_canceled, 0.0f);
            x0 += canceled_w;
        }
        if (remain_w > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + remain_w, p.y + sz.y), col_remain);
        }

        char label[128]{ 0 };
        std::snprintf(label, sizeof(label), "ok:%lld rem:%lld fail:%lld can:%lld done:%lld/%lld",
            (long long)success_clamped,
            (long long)remain,
            (long long)failed_clamped,
            (long long)canceled_clamped,
            (long long)completed,
            (long long)total);

        const ImVec2 text_sz = ImGui::CalcTextSize(label);
        const ImVec2 text_pos(
            p.x + (sz.x - text_sz.x) * 0.5f,
            p.y + (sz.y - text_sz.y) * 0.5f);
        const ImU32 text_shadow = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        const ImU32 text_fg = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.98f));
        dl->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), text_shadow, label);
        dl->AddText(text_pos, text_fg, label);
    }

    static void kick_family_fetch(const std::vector<JobSetLite>& seeds) {
        auto& s = S();
        std::vector<int64_t> ids;
        ids.reserve(seeds.size());
        for (const auto& r : seeds) ids.push_back(r.job_set_id);
        s.family_in_flight = true;
        s.fut_family = simcore::db::DataService::FetchJobSetFamiliesForSeedsAsync(ids);
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
        if (s.before || s.after) return;
        if (s.fetch_in_flight || s.family_in_flight) return;
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
            s.family_items = s.page.items;
            kick_family_fetch(s.page.items);
        }
        else {
            s.page = {};
            s.family_items.clear();
        }
    }

    static void consume_family_if_ready() {
        auto& s = S();
        if (!s.family_in_flight) return;
        using namespace std::chrono_literals;
        if (!s.fut_family.valid()) return;
        if (s.fut_family.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_family.get();
        s.family_in_flight = false;
        if (r.ok) {
            s.family_items = std::move(r.value);
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

    static void prune_expansion_state(const std::vector<JobSetLite>& rows) {
        auto& s = S();
        std::unordered_set<int64_t> present;
        present.reserve(rows.size());
        for (const auto& r : rows) present.insert(r.job_set_id);
        for (auto it = s.expanded_job_set_ids.begin(); it != s.expanded_job_set_ids.end();) {
            if (present.find(*it) == present.end()) it = s.expanded_job_set_ids.erase(it);
            else ++it;
        }
    }

    static void consume_boost_if_ready() {
        auto& s = S();
        if (!s.boost_in_flight) return;
        using namespace std::chrono_literals;
        if (!s.fut_boost.valid()) return;
        if (s.fut_boost.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_boost.get();
        s.boost_in_flight = false;
        if (r.ok) {
            const std::string msg =
                std::to_string((long long)r.value.changed_jobs) + " jobs set to priority " + std::to_string(r.value.new_priority);
            GuiToastBus::Success(
                "Job set boosted",
                msg.c_str());
            s.before.reset();
            s.after.reset();
            kick_fetch();
        }
        else {
            GuiToastBus::Error("Boost failed", r.error.message);
        }
    }
    static void consume_cancel_tree_if_ready() {
        auto& s = S();
        if (!s.cancel_tree_in_flight) return;
        using namespace std::chrono_literals;
        if (!s.fut_cancel_tree.valid()) return;
        if (s.fut_cancel_tree.wait_for(0ms) != std::future_status::ready) return;

        auto r = s.fut_cancel_tree.get();
        s.cancel_tree_in_flight = false;
        if (r.ok) {
            const std::string msg = std::to_string((long long)r.value.canceled_job_ids.size()) + " queued jobs canceled";
            GuiToastBus::Warn("Job set queue canceled", msg.c_str());
            s.before.reset();
            s.after.reset();
            kick_fetch();
        }
        else {
            GuiToastBus::Error("Cancel queued failed", r.error.message);
        }
    }

}

void JobSetsPane::OnActivated() {
    auto& s = S();
    s.program_names.clear();
    kick_kinds_fetch();
    if (!s.fetch_in_flight && s.family_items.empty()) kick_fetch();
}

void JobSetsPane::Draw() {
    auto& s = S();

    ImGui::Begin("Job Sets", nullptr, ImGuiWindowFlags_NoMove);

    static int selected_kind = -1;
    static std::optional<JobSetStateFilter> selected_state{};
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
    if (ImGui::BeginCombo("State", state_filter_label(selected_state))) {
        const bool all_selected = !selected_state.has_value();
        if (ImGui::Selectable("All", all_selected)) selected_state.reset();

        bool completed_selected = selected_state == JobSetStateFilter::Completed;
        if (ImGui::Selectable("Completed", completed_selected)) selected_state = JobSetStateFilter::Completed;

        bool incomplete_selected = selected_state == JobSetStateFilter::Incomplete;
        if (ImGui::Selectable("Incomplete", incomplete_selected)) selected_state = JobSetStateFilter::Incomplete;

        bool failed_selected = selected_state == JobSetStateFilter::HasFailures;
        if (ImGui::Selectable("Has failures", failed_selected)) selected_state = JobSetStateFilter::HasFailures;

        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        s.scope = {};
        if (selected_kind >= 0) s.scope.program_kind = selected_kind;
        s.scope.state_filter = selected_state;
        s.before.reset();
        s.after.reset();
        s.page_limit = page_sz;
        s.auto_refresh = auto_ref;
        s.refresh_seconds = refresh_sec;
        s.family_items.clear();
        kick_fetch();
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        selected_kind = -1;
        selected_state.reset();
        s.scope = {};
        s.before.reset();
        s.after.reset();
        s.page_limit = 100;
        page_sz = s.page_limit;
        s.auto_refresh = true;
        auto_ref = true;
        s.refresh_seconds = 2;
        refresh_sec = 2;
        s.family_items.clear();
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
        s.family_items.clear();
        kick_fetch();
    }
    if (!has_prev) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!has_next) ImGui::BeginDisabled();
    if (ImGui::Button("Next")) {
        s.before = s.page.next;
        s.after.reset();
        s.family_items.clear();
        kick_fetch();
    }
    if (!has_next) ImGui::EndDisabled();

    ImGui::Separator();

    consume_fetch_if_ready();
    consume_family_if_ready();
    consume_kinds_fetch_if_ready();
    consume_delete_if_ready();
    consume_boost_if_ready();
    consume_cancel_tree_if_ready();
    maybe_refresh();

    if (ImGui::BeginTable("JobSetsTable", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("job_set_id");
        ImGui::TableSetupColumn("program_kind");
        ImGui::TableSetupColumn("purpose", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("created_at");
        ImGui::TableSetupColumn("progress", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("open_jobs");
        ImGui::TableHeadersRow();

        const auto& rows = s.family_items;
        prune_expansion_state(rows);

        std::unordered_map<int64_t, const JobSetLite*> by_id;
        std::unordered_map<int64_t, std::vector<const JobSetLite*>> children;
        std::vector<const JobSetLite*> roots;
        by_id.reserve(rows.size());
        children.reserve(rows.size());

        for (const auto& r : rows) by_id[r.job_set_id] = &r;
        for (const auto& r : rows) {
            if (r.parent_job_set_id.has_value()) {
                auto pit = by_id.find(*r.parent_job_set_id);
                if (pit != by_id.end()) {
                    children[*r.parent_job_set_id].push_back(&r);
                    continue;
                }
            }
            roots.push_back(&r);
        }

        auto sorter = [](const JobSetLite* a, const JobSetLite* b) {
            if (a->created_at != b->created_at) return a->created_at > b->created_at;
            return a->job_set_id > b->job_set_id;
        };
        std::sort(roots.begin(), roots.end(), sorter);
        for (auto& kv : children) std::sort(kv.second.begin(), kv.second.end(), sorter);

        std::unordered_set<int64_t> active;
        active.reserve(rows.size());

        std::function<void(const JobSetLite*)> draw_node = [&](const JobSetLite* r) {
            if (!r) return;
            if (active.find(r->job_set_id) != active.end()) return;
            active.insert(r->job_set_id);

            ImGui::PushID((int)r->job_set_id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            auto itc = children.find(r->job_set_id);
            const bool has_children = (itc != children.end() && !itc->second.empty());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_AllowItemOverlap;
            if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            const bool remembered_open = s.expanded_job_set_ids.find(r->job_set_id) != s.expanded_job_set_ids.end();
            ImGui::SetNextItemOpen(remembered_open, ImGuiCond_Always);
            bool is_open = ImGui::TreeNodeEx("##node", flags, "%lld", (long long)r->job_set_id);
            if (has_children && ImGui::IsItemToggledOpen()) {
                if (is_open) s.expanded_job_set_ids.insert(r->job_set_id);
                else s.expanded_job_set_ids.erase(r->job_set_id);
            }

            ImGui::TableSetColumnIndex(1);
            {
                auto it = s.program_names.find(r->program_kind);
                if (it != s.program_names.end()) ImGui::TextUnformatted(it->second.c_str());
                else ImGui::Text("kind %d", r->program_kind);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r->purpose.empty() ? "(none)" : r->purpose.c_str());

            ImGui::TableSetColumnIndex(3);
            {
                auto ts = fmt_time(r->created_at);
                ImGui::TextUnformatted(ts.c_str());
            }

            ImGui::TableSetColumnIndex(4);
            {
                draw_segmented_progress(r->succeeded_jobs, r->failed_jobs, r->canceled_jobs, r->completed_jobs, r->total_jobs);
            }

            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Open Jobs")) {
                JobsPane::FocusJobSet(r->job_set_id);
                GuiLeftNav::SetActive(GuiPane::Jobs);
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(s.delete_in_flight || s.boost_in_flight || s.cancel_tree_in_flight);
            if (ImGui::SmallButton("Boost")) {
                s.boost_in_flight = true;
                s.fut_boost = simcore::db::DataService::BoostJobSetPriorityTreeAsync(r->job_set_id);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(s.delete_in_flight || s.boost_in_flight || s.cancel_tree_in_flight);
            if (ImGui::SmallButton("Cancel queued")) {
                s.cancel_tree_in_flight = true;
                s.fut_cancel_tree = simcore::db::DataService::CancelQueuedJobsForJobSetTreeAsync(r->job_set_id);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(s.delete_in_flight || s.cancel_tree_in_flight);
            if (ImGui::SmallButton("Delete")) {
                s.pending_delete_job_set_id = r->job_set_id;
                s.request_delete_modal_open = true;
            }
            ImGui::EndDisabled();

            if (has_children && is_open) {
                for (const auto* c : itc->second) draw_node(c);
                ImGui::TreePop();
            }

            ImGui::PopID();
            active.erase(r->job_set_id);
        };

        for (const auto* r : roots) draw_node(r);

        ImGui::EndTable();
    }

    if (s.request_delete_modal_open) {
        ImGui::OpenPopup(kDeleteJobSetPopupId);
        s.request_delete_modal_open = false;
    }

    if (ImGui::BeginPopupModal(kDeleteJobSetPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
