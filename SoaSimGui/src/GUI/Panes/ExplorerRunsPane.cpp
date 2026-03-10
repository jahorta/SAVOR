#include "ExplorerRunsPane.h"

#include "imgui.h"
#include "DB/Querying/DataService.h"
#include "DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "Phases/Programs/BattleRunner/BattleOutcome.h"
#include "Runner/IPC/Wire.h"
#include "misc/cpp/imgui_stdlib.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobEventsRepo.h"
#include "DB/ExplorerSettingsRepo.h"
#include "DB/SeedProbeRepo.h"
#include "DB/Querying/PagedQuery.h"
#include "Utils/IniDoc.h"
#include "../../Components/ToastBus.h"

#include <future>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <ctime>

using namespace simcore::db;
using namespace std::chrono;

namespace {
    struct WaveMeta {
        int64_t root_group_id{-1};
        uint32_t wave_turn{1};
        int64_t settings_id{-1};
        int64_t seed_probe_id{-1};
        std::string settings_name;
        int64_t tas_movie_id{-1};

        static WaveMeta from_text(const std::optional<std::string>& t) {
            WaveMeta m{};
            if (!t.has_value() || t->empty()) return m;
            IniDoc ini = IniDoc::parse(*t);
            constexpr const char* sec = "BattleSingleTurn.WaveMeta";
            if (!ini.has_section(sec)) return m;
            auto kv = ini.section_kv(sec);
            m.root_group_id = kv.get_i64("root_group_id", -1);
            m.wave_turn = kv.get_u32("wave_turn", 1);
            m.settings_id = kv.get_i64("settings_id", -1);
            m.seed_probe_id = kv.get_i64("seed_probe_id", -1);
            m.settings_name = kv.get("settings_name", "");
            m.tas_movie_id = kv.get_i64("tas_movie_id", -1);
            return m;
        }
    };

    struct WaveRow {
        int64_t job_set_id{};
        int64_t created_at{};
        uint32_t wave_turn{1};
        std::string status_summary;
    };

    struct GroupRow {
        int64_t root_group_id{};
        std::string tas_movie;
        std::string settings_label;
        int64_t created_at{};
        int total_waves{};
        std::string status_summary;
        std::vector<WaveRow> waves;
    };

    struct JobViewRow {
        int64_t job_id{};
        std::string state;
        uint32_t fake_used{};
    };

    struct State {
        std::future<DbResult<std::vector<GroupRow>>> fut_groups;
        bool groups_in_flight{false};
        std::vector<GroupRow> groups;

        int64_t selected_root{-1};
        int64_t selected_wave{-1};

        std::future<DbResult<std::vector<JobViewRow>>> fut_jobs;
        bool jobs_in_flight{false};
        std::vector<JobViewRow> jobs;
        int64_t selected_job{-1};

        std::future<DbResult<IniDoc>> fut_results;
        std::future<DbResult<std::string>> fut_progress;
        bool details_in_flight{false};
        std::string results_log;
        std::string progress_log;

        bool auto_refresh{true};
        int refresh_seconds{3};
        steady_clock::time_point last_fetch{};
    };

    static State& S() { static State s; return s; }

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

    static std::string summarize_states(const std::vector<JobRow>& jobs) {
        int q = 0, r = 0, f = 0, w = 0, d = 0;
        for (auto& j : jobs) {
            if (j.state == "QUEUED" || j.state == "CLAIMED") ++q;
            else if (j.state == "RUNNING") ++r;
            else if (j.state == "FAILED" || j.state == "CANCELED") ++f;
            else if (j.state == "SUCCEEDED_WINNER") ++w;
            else if (j.state == "SUCCEEDED_DUPLICATE") ++d;
        }
        return "Q:" + std::to_string(q) + " R:" + std::to_string(r) + " F:" + std::to_string(f) + " W:" + std::to_string(w) + " D:" + std::to_string(d);
    }

    static int64_t resolve_root(const JobSetRow& js) {
        WaveMeta m = WaveMeta::from_text(js.meta_text);
        if (m.root_group_id > 0) return m.root_group_id;

        int64_t cur = js.job_set_id;
        while (true) {
            auto pr = JobSetsRepo::GetParent(cur);
            if (!pr.ok || !pr.value.has_value()) break;
            cur = *pr.value;
        }
        return cur;
    }

    static std::vector<GroupRow> build_groups() {
        JobSetsListScope scope{};
        scope.program_kind = simcore::PK_BattleSingleTurnRunner;
        auto page = JobSetsRepo::ListRecentAsync(scope, std::nullopt, 400).get();
        if (!page.ok) return {};

        std::unordered_map<int64_t, GroupRow> gmap;
        for (auto& lite : page.value.items) {
            auto js = JobSetsRepo::Get(lite.job_set_id);
            if (!js.ok) continue;

            WaveMeta meta = WaveMeta::from_text(js.value.meta_text);
            int64_t root_id = resolve_root(js.value);

            auto& g = gmap[root_id];
            g.root_group_id = root_id;
            if (g.created_at == 0 || js.value.created_at < g.created_at) g.created_at = js.value.created_at;

            std::string settings = "settings_id=" + std::to_string(meta.settings_id);
            if (!meta.settings_name.empty()) settings = meta.settings_name;
            g.settings_label = settings;

            if (meta.tas_movie_id > 0) g.tas_movie = std::to_string(meta.tas_movie_id);
            else g.tas_movie = "unknown";

            auto jobs = JobsRepo::GetByJobSet(js.value.job_set_id);
            std::string status = jobs.ok ? summarize_states(jobs.value) : "(error)";

            g.waves.push_back({ js.value.job_set_id, js.value.created_at, meta.wave_turn, status });
        }

        std::vector<GroupRow> groups;
        groups.reserve(gmap.size());
        for (auto& kv : gmap) {
            auto& g = kv.second;
            std::sort(g.waves.begin(), g.waves.end(), [](const WaveRow& a, const WaveRow& b) {
                if (a.wave_turn != b.wave_turn) return a.wave_turn < b.wave_turn;
                return a.created_at < b.created_at;
            });
            g.total_waves = (int)g.waves.size();
            g.status_summary = g.waves.empty() ? "" : g.waves.back().status_summary;
            groups.push_back(g);
        }

        std::sort(groups.begin(), groups.end(), [](const GroupRow& a, const GroupRow& b) {
            return a.created_at > b.created_at;
        });

        return groups;
    }

    static std::vector<JobViewRow> build_jobs_for_wave(int64_t wave_job_set_id) {
        std::vector<JobViewRow> out;
        auto jobs = JobsRepo::GetByJobSet(wave_job_set_id);
        if (!jobs.ok) return out;

        std::vector<int64_t> ids;
        ids.reserve(jobs.value.size());
        for (auto& j : jobs.value) ids.push_back(j.job_id);

        std::unordered_map<int64_t, uint32_t> fake_used;
        auto res = JobEventsRepo::GetLatestPayloadByJobs(ids, "RESULTS");
        if (res.ok) {
            for (auto& r : res.value) {
                if (!r.payload.has_value()) continue;
                IniDoc ini = IniDoc::parse(*r.payload);
                if (!ini.has_section("BattleSingleTurn.Results")) continue;
                fake_used[r.job_id] = ini.section_kv("BattleSingleTurn.Results").get_u32("fake_attacks_used", 0);
            }
        }

        for (auto& j : jobs.value) {
            out.push_back({ j.job_id, j.state, fake_used.count(j.job_id) ? fake_used[j.job_id] : 0 });
        }
        std::sort(out.begin(), out.end(), [](const JobViewRow& a, const JobViewRow& b) { return a.job_id < b.job_id; });
        return out;
    }

    static void kick_groups_fetch() {
        auto& s = S();
        if (s.groups_in_flight) return;
        s.groups_in_flight = true;
        s.last_fetch = steady_clock::now();
        s.fut_groups = std::async(std::launch::async, []() -> DbResult<std::vector<GroupRow>> {
            return DbResult<std::vector<GroupRow>>::Ok(build_groups());
        });
    }

    static void kick_jobs_fetch(int64_t wave_job_set_id) {
        auto& s = S();
        if (s.jobs_in_flight) return;
        s.jobs_in_flight = true;
        s.fut_jobs = std::async(std::launch::async, [wave_job_set_id]() -> DbResult<std::vector<JobViewRow>> {
            return DbResult<std::vector<JobViewRow>>::Ok(build_jobs_for_wave(wave_job_set_id));
        });
    }

    static void kick_details_fetch(int64_t job_id) {
        auto& s = S();
        s.details_in_flight = true;
        s.fut_results = DataService::FetchJobResultsIniAsync(job_id);
        s.fut_progress = std::async(std::launch::async, [job_id]() -> DbResult<std::string> {
            auto p = JobEventsRepo::ListByJobAndKind(job_id, "PROGRESS");
            if (!p.ok) return DbResult<std::string>::Err(p.error);
            std::string out;
            for (auto& e : p.value) {
                if (e.payload.has_value()) {
                    out += *e.payload;
                    out += "\n";
                }
            }
            return DbResult<std::string>::Ok(std::move(out));
        });
    }

    static void consume_fetches() {
        auto& s = S();
        if (s.groups_in_flight && s.fut_groups.valid() && s.fut_groups.wait_for(0ms) == std::future_status::ready) {
            auto r = s.fut_groups.get();
            s.groups_in_flight = false;
            if (r.ok) s.groups = std::move(r.value);
        }
        if (s.jobs_in_flight && s.fut_jobs.valid() && s.fut_jobs.wait_for(0ms) == std::future_status::ready) {
            auto r = s.fut_jobs.get();
            s.jobs_in_flight = false;
            if (r.ok) s.jobs = std::move(r.value);
        }

        if (s.details_in_flight && s.fut_results.valid() && s.fut_progress.valid()
            && s.fut_results.wait_for(0ms) == std::future_status::ready
            && s.fut_progress.wait_for(0ms) == std::future_status::ready) {
            auto rr = s.fut_results.get();
            auto pr = s.fut_progress.get();
            s.details_in_flight = false;
            s.results_log = rr.ok ? rr.value.to_string_sorted() : "(no results)";
            s.progress_log = pr.ok ? pr.value : "(no progress)";
        }

        if (s.auto_refresh && !s.groups_in_flight) {
            if (duration_cast<seconds>(steady_clock::now() - s.last_fetch).count() >= s.refresh_seconds) {
                kick_groups_fetch();
            }
        }
    }

    static GroupRow* selected_group() {
        auto& s = S();
        for (auto& g : s.groups) if (g.root_group_id == s.selected_root) return &g;
        return nullptr;
    }

    static bool selected_job_can_trigger() {
        auto& s = S();
        if (s.selected_job <= 0) return false;
        auto jr = JobsRepo::Get(s.selected_job);
        if (!jr.ok) return false;
        auto rr = JobEventsRepo::GetLatestPayload(s.selected_job, "RESULTS");
        if (!rr.ok || !rr.value.has_value()) return false;

        IniDoc ini = IniDoc::parse(*rr.value);
        if (!ini.has_section("BattleSingleTurn.Results")) return false;
        auto kv = ini.section_kv("BattleSingleTurn.Results");
        if (kv.get_i64("output_savestate_id", -1) <= 0) return false;
        if (kv.get_u32("battle_outcome", 0) != (uint32_t)simcore::battle::Outcome::ReachedNextTurn) return false;
        return true;
    }
}

void ExplorerRunsPane::OnActivated() {
    auto& s = S();
    s.selected_root = -1;
    s.selected_wave = -1;
    s.selected_job = -1;
    s.groups.clear();
    s.jobs.clear();
    s.results_log.clear();
    s.progress_log.clear();
    kick_groups_fetch();
}

void ExplorerRunsPane::Draw() {
    auto& s = S();
    consume_fetches();

    ImGui::Begin("Explorer Runs", nullptr, ImGuiWindowFlags_NoMove);

    if (ImGui::Button("Refresh")) {
        kick_groups_fetch();
        if (s.selected_wave > 0) kick_jobs_fetch(s.selected_wave);
        if (s.selected_job > 0) kick_details_fetch(s.selected_job);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &s.auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("Every (s)", &s.refresh_seconds, 1, 10);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float col_l = avail_w * 0.32f;
    const float col_c = avail_w * 0.24f;

    ImGui::BeginChild("left_groups", ImVec2(col_l, 0), true);
    ImGui::TextUnformatted("Run Groups");
    ImGui::Separator();
    if (ImGui::BeginTable("groups_tbl", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Root Group ID");
        ImGui::TableSetupColumn("Tas Movie");
        ImGui::TableSetupColumn("Explorer Settings");
        ImGui::TableSetupColumn("Created");
        ImGui::TableSetupColumn("Total Waves");
        ImGui::TableSetupColumn("Status Summary");
        ImGui::TableHeadersRow();

        for (auto& g : s.groups) {
            ImGui::PushID((int)g.root_group_id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool sel = (s.selected_root == g.root_group_id);
            if (ImGui::Selectable(std::to_string((long long)g.root_group_id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                s.selected_root = g.root_group_id;
                s.selected_wave = -1;
                s.selected_job = -1;
                s.jobs.clear();
                s.results_log.clear();
                s.progress_log.clear();
            }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(g.tas_movie.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(g.settings_label.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(fmt_time(g.created_at).c_str());
            ImGui::TableSetColumnIndex(4); ImGui::Text("%d", g.total_waves);
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(g.status_summary.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("center_tree", ImVec2(col_c, 0), true);
    ImGui::TextUnformatted("Wave Tree");
    ImGui::Separator();

    if (auto* g = selected_group()) {
        std::unordered_map<uint32_t, std::vector<WaveRow*>> by_turn;
        for (auto& w : g->waves) by_turn[w.wave_turn].push_back(&w);

        std::vector<uint32_t> turns;
        turns.reserve(by_turn.size());
        for (auto& kv : by_turn) turns.push_back(kv.first);
        std::sort(turns.begin(), turns.end());

        for (auto t : turns) {
            std::string label = "Turn " + std::to_string(t);
            if (ImGui::TreeNode(label.c_str())) {
                auto& ws = by_turn[t];
                std::sort(ws.begin(), ws.end(), [](const WaveRow* a, const WaveRow* b) { return a->created_at < b->created_at; });
                for (auto* w : ws) {
                    bool sel = s.selected_wave == w->job_set_id;
                    std::string wlabel = "Wave " + std::to_string((long long)w->job_set_id);
                    if (ImGui::Selectable(wlabel.c_str(), sel)) {
                        s.selected_wave = w->job_set_id;
                        s.selected_job = -1;
                        s.results_log.clear();
                        s.progress_log.clear();
                        kick_jobs_fetch(s.selected_wave);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", w->status_summary.c_str());
                }
                ImGui::TreePop();
            }
        }
    }

    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right_split", ImVec2(0, 0), true);
    ImGui::Columns(2, "right_cols", true);

    ImGui::TextUnformatted("Wave Jobs");
    ImGui::Separator();
    if (ImGui::BeginTable("jobs_tbl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Job ID");
        ImGui::TableSetupColumn("Job State");
        ImGui::TableSetupColumn("Cumulative Fake Attacks");
        ImGui::TableHeadersRow();

        for (auto& j : s.jobs) {
            ImGui::PushID((int)j.job_id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool sel = s.selected_job == j.job_id;
            if (ImGui::Selectable(std::to_string((long long)j.job_id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                s.selected_job = j.job_id;
                kick_details_fetch(j.job_id);
            }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(j.state.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", j.fake_used);
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::NextColumn();

    ImGui::TextUnformatted("Details");
    ImGui::Separator();
    bool can_trigger = selected_job_can_trigger();
    ImGui::BeginDisabled(!can_trigger);
    if (ImGui::Button("Trigger Next Wave") && s.selected_job > 0) {
        auto r = BattleSingleTurnRunDBCodec::enqueue_next_wave_from_job(s.selected_job);
        if (r.ok) {
            GuiToastBus::Info("Queued next wave job set " + std::to_string(r.value));
            kick_groups_fetch();
        }
        else {
            GuiToastBus::Error("Unable to queue next wave", r.error.message);
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Results Log");
    ImGui::InputTextMultiline("##results", &s.results_log, ImVec2(-1, ImGui::GetTextLineHeight() * 8), ImGuiInputTextFlags_ReadOnly);

    ImGui::SeparatorText("Progress Log");
    ImGui::InputTextMultiline("##progress", &s.progress_log, ImVec2(-1, 0), ImGuiInputTextFlags_ReadOnly);

    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::End();
}
