#include "PhaseBuilderPane.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "DB/Querying/DataService.h"
#include "Runner/IPC/Wire.h"
#include "Utils/IniDoc.h"
#include "Utils/IniKV.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderService.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderPreview.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderSchemas.h"
#include "../Popups/IdPicker.h"            // your generic picker widget
#include "../Popups/IdRepoAdapters.h"      // Make*Adapter(...) factories
#include "../../Components/FutureQueue.h"
#include "../../Components/ToastBus.h"

using simcore::db::DbResult;
using simcore::db::ProgramKindKV;
using simcore::db::phasebuilder::PhasePreview;

namespace {
    enum class SubmitState { Idle, Working, Done, Error };
    enum class PreviewState { Idle, Working, Ready, Error };

    // UI-only inputs when coordinator is stopped
    struct UiValues {
        // state
        bool kinds_requested_ = false;
        std::future<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> kinds_future_;
        std::vector<simcore::db::ProgramKindKV> kinds_;
        int selected_kind_idx_ = -1;

        std::optional<IniDoc> ini_;
        bool ini_dirty_ = false;

        std::string purpose_;
        std::optional<std::string> meta_text_;

        std::vector<std::pair<std::string, std::string>> validation_errors_;

        PreviewState preview_state_ = PreviewState::Idle;
        std::future<simcore::db::DbResult<simcore::db::phasebuilder::PhasePreview>> preview_future_;
        std::optional<simcore::db::phasebuilder::PhasePreview> preview_;
        std::string preview_err_;

        int battle_plan_count_ = -1;
        int unique_seed_count_ = -1;

        SubmitState submit_state_ = SubmitState::Idle;
        std::future<simcore::db::DbResult<std::pair<int64_t, int>>> submit_future_;
        std::optional<int64_t> created_job_set_id_;
        std::string submit_err_;
    };
    UiValues& inst() { static UiValues s; return s; }
}

static int kind_to_index(const std::vector<ProgramKindKV>& v, int pk) {
    for (int i = 0; i < (int)v.size(); ++i) if (v[i].id == pk) return i;
    return v.empty() ? -1 : 0;
}

void PhaseBuilderPane::ensureKindsLoaded() {
    if (inst().kinds_requested_) return;
    inst().kinds_requested_ = true;
    inst().kinds_future_ = simcore::db::DataService::ListProgramKindsAsync({});
}

void PhaseBuilderPane::ensureDefaults() {
    if (!inst().ini_.has_value() && inst().selected_kind_idx_ >= 0 && inst().selected_kind_idx_ < (int)inst().kinds_.size()) {
        const int pk = inst().kinds_[inst().selected_kind_idx_].id;
        inst().ini_ = simcore::db::phasebuilder::PhaseBuilderService::DefaultsFor(pk);
        inst().ini_dirty_ = false;
        inst().submit_state_ = SubmitState::Idle;
    }
}

void PhaseBuilderPane::drawKindPicker() {
    ensureKindsLoaded();

    if (inst().kinds_future_.valid() && inst().kinds_.empty()) {
        auto st = inst().kinds_future_.wait_for(std::chrono::seconds(0));
        if (st == std::future_status::ready) {
            auto r = inst().kinds_future_.get();
            if (r.ok) {
                inst().kinds_ = std::move(r.value);
                inst().selected_kind_idx_ = kind_to_index(inst().kinds_, simcore::PK_SeedProbe);
            }
        }
    }

    ImGui::TextUnformatted("Program Kind");
    if (ImGui::BeginCombo("##pk", (inst().selected_kind_idx_ >= 0 && 
        inst().selected_kind_idx_ < (int)inst().kinds_.size()) ? 
        inst().kinds_[inst().selected_kind_idx_].name.c_str() : "(loading)")) {

        for (int i = 0; i < (int)inst().kinds_.size(); ++i) {
            bool sel = (i == inst().selected_kind_idx_);
            if (ImGui::Selectable(inst().kinds_[i].name.c_str(), sel)) {
                inst().selected_kind_idx_ = i;
                inst().ini_.reset();
                inst().preview_.reset();
                inst().preview_state_ = PreviewState::Idle;
                inst().validation_errors_.clear();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void PhaseBuilderPane::drawSeedProbeForm() {
    using namespace simcore::db::codec::seedprobe;
    ensureDefaults();
    if (!inst().ini_) return;

    auto bp = BlueprintIni::from_section(*inst().ini_);
    auto grid = GridIni::from_section(*inst().ini_);
    auto uni = UniqueIni::from_section(*inst().ini_);

    ImGui::SeparatorText("SeedProbe - General");
    int64_t savestate_id = bp.savestate_id;
    int priority = bp.priority;
    int run_ms = (int)bp.run_ms;
    int vi_ms = (int)bp.vi_stall_ms;
    bool clear_winners = bp.clear_result_winners;
    bool auto_queue_br = bp.auto_schedule_battle_run;

    
    ImGui::Text("savestate_id %lld", savestate_id); ImGui::SameLine();
    if (ImGui::Button("Pick...")) { ImGui::OpenPopup("PB_SavestatePicker"); }

    // Modal picker (keeps state alive across frames)
    {
        // NOTE: RowT must match what your adapter uses (Lite vs Row). If your adapter returns *Lite*, use SavestateLite.
        using RowT = simcore::db::SavestateLite; // or: simcore::db::SavestateRow
        static soasim::ui::LedgerPicker<RowT> picker;
        static bool init = false;
        if (!init) {
            picker.adapter = soasim::ui::adapters::MakeSavestateAdapter(/*page_size*/100);
            picker.args.modal_id = "PB_SavestatePicker";
            picker.args.initial_query.limit = picker.adapter.page_size;
            picker.args.initial_query.order = PageOrder::Desc;
            picker.open = false;
            init = true;
        }
        picker.open = ImGui::IsPopupOpen("PB_SavestatePicker");
        picker.Draw([&](const soasim::ui::PickResult& pr, const std::optional<RowT>& row) {
            if (pr.ok) {
                savestate_id = pr.id;
                inst().ini_dirty_ = true; // will persist via bp.set_section(*ini_) at the bottom
            }
            });
    }

    // Labels for next row
    ImGui::Text("priority             ");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("run_ms                     ");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("vi_stall_ms");

    // Next row
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##priority", &priority)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##run_ms", &run_ms)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##vi_stall_ms", &vi_ms)) { inst().ini_dirty_ = true; }


    if (ImGui::Checkbox("clear_result_winners", &clear_winners)) { inst().ini_dirty_ = true; }
    //if (ImGui::Checkbox("auto_schedule_battle_run", &auto_queue_br)) { inst().ini_dirty_ = true; }

    ImGui::SeparatorText("SeedProbe - Grid");
    int spa = grid.samples_per_axis;
    int minv = (int)grid.min_value;
    int maxv = (int)grid.max_value;
    bool cap_top = grid.cap_trigger_top;
    bool ignore_trig_mm = grid.ignore_trigger_minmax;

    //Labels for next row
    ImGui::Text("samples_per_axis");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("min_value           "); 

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("max_value");

    //Next row
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("##samples_per_axis", &spa)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine(); 
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("##min_value", &minv)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine(); 
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("##max_value", &maxv)) { inst().ini_dirty_ = true; }

    if (ImGui::Checkbox("cap_trigger_top", &cap_top)) { inst().ini_dirty_ = true; }

    ImGui::SameLine();
    if (ImGui::Checkbox("ignore_trigger_minmax", &ignore_trig_mm)) { inst().ini_dirty_ = true; }

    ImGui::SeparatorText("SeedProbe - Unique");
    int cat = uni.combo_attempts_per_target;
    int cst = uni.combo_sampler_tries;

    //Labels for next row
    ImGui::Text("combo_attempts_per_target");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("combo_sampler_tries");

    //Next Row
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("##combo_attempts_per_target", &cat)) { inst().ini_dirty_ = true; }
    
    ImGui::SameLine(); ImGui::Text("                | "); ImGui::SameLine();
    ImGui::SameLine(); ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##combo_sampler_tries", &cst)) { inst().ini_dirty_ = true; }

    if (inst().ini_dirty_) {
        bp.savestate_id = savestate_id;
        bp.priority = priority;
        bp.run_ms = (uint32_t)run_ms;
        bp.vi_stall_ms = (uint32_t)vi_ms;
        bp.clear_result_winners = clear_winners;
        bp.auto_schedule_battle_run = auto_queue_br;
        bp.set_section(*inst().ini_);

        grid.samples_per_axis = spa;
        grid.min_value = (uint8_t)std::clamp(minv, 0, 255);
        grid.max_value = (uint8_t)std::clamp(maxv, 0, 255);
        grid.cap_trigger_top = cap_top;
        grid.ignore_trigger_minmax = ignore_trig_mm;
        grid.set_section(*inst().ini_);

        uni.combo_attempts_per_target = cat;
        uni.combo_sampler_tries = cst;
        uni.set_section(*inst().ini_);
    }
}

void PhaseBuilderPane::drawTasMovieForm() {
    using namespace simcore::db::codec::tas;
    ensureDefaults();
    if (!inst().ini_) return;

    auto bp = BlueprintIni::from_section(*inst().ini_);

    ImGui::SeparatorText("TasMovie");
    int64_t artifact_id = bp.base_dtm_artifact_id;
    int64_t rtc_low = bp.rtc_low;
    int64_t rtc_high = bp.rtc_high;
    int priority = bp.priority;
    int run_ms = (int)bp.run_ms;
    int vi_ms = (int)bp.vi_stall_ms;
    bool progress_enable = bp.progress_enable;
    bool auto_queue = bp.auto_queue_seeds;

    ImGui::Text("base_dtm_artifact_id %lld", artifact_id); ImGui::SameLine();
    if (ImGui::Button("Pick...")) { ImGui::OpenPopup("PB_ObjectRefPicker"); }

    {
        using RowT = simcore::db::ObjectRefLite; // or: simcore::db::ObjectRefRow
        static soasim::ui::LedgerPicker<RowT> picker;
        static bool init = false;
        if (!init) {
            picker.adapter = soasim::ui::adapters::MakeObjectRefAdapter(".dtm" /*ext filter*/, /*page_size*/100);
            picker.args.modal_id = "PB_ObjectRefPicker";
            picker.args.initial_query.limit = picker.adapter.page_size;
            picker.args.initial_query.order = PageOrder::Desc;
            init = true;
        }
        picker.open = ImGui::IsPopupOpen("PB_ObjectRefPicker");
        picker.Draw([&](const soasim::ui::PickResult& pr, const std::optional<RowT>& row) {
            if (pr.ok) {
                artifact_id = pr.id;
                inst().ini_dirty_ = true;
            }
            });
    }

    if (ImGui::InputScalar("rtc_low", ImGuiDataType_S64, &rtc_low)) { inst().ini_dirty_ = true; }
    if (ImGui::InputScalar("rtc_high", ImGuiDataType_S64, &rtc_high)) { inst().ini_dirty_ = true; }
    // Labels for next row
    ImGui::Text("priority             ");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("run_ms                     ");

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::Text("vi_stall_ms");

    // Next row
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##priority", &priority)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##run_ms", &run_ms)) { inst().ini_dirty_ = true; }

    ImGui::SameLine(); ImGui::Text(" | "); ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("##vi_stall_ms", &vi_ms)) { inst().ini_dirty_ = true; }

    if (ImGui::Checkbox("progress_enable", &progress_enable)) { inst().ini_dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("auto_queue_seeds", &auto_queue)) { inst().ini_dirty_ = true; }

    if (inst().ini_dirty_) {
        bp.base_dtm_artifact_id = artifact_id;
        bp.rtc_low = rtc_low;
        bp.rtc_high = rtc_high;
        bp.priority = priority;
        bp.run_ms = (uint32_t)run_ms;
        bp.vi_stall_ms = (uint32_t)vi_ms;
        bp.progress_enable = progress_enable;
        bp.auto_queue_seeds = auto_queue;
        bp.set_section(*inst().ini_);
    }
}

void PhaseBuilderPane::drawExplorerRunForm() {
    ensureDefaults();
    if (!inst().ini_) return;

    auto bp = simcore::db::codec::battle::run::BlueprintIni::from_section(*inst().ini_);

    ImGui::SeparatorText("ExplorerRun");
    int64_t settings_id = bp.settings_id;
    int64_t seed_probe_id = bp.seed_probe_id;
    int priority = bp.priority;
    int run_ms = (int)bp.run_ms;
    int vi_ms = (int)bp.vi_stall_ms;
    bool progress_enable = bp.progress_enable;

    if (ImGui::Button("Pick SeedProbe...")) {
        ImGui::OpenPopup("PB_SeedProbe");
    }

    {
        using RowT = simcore::db::SeedProbeLite; // or: simcore::db::BattleRunGroupRow
        static soasim::ui::LedgerPicker<RowT> picker;
        static bool init = false;
        if (!init) {
            picker.adapter = soasim::ui::adapters::MakeSeedProbeAdapter(/*page_size*/100);
            picker.args.modal_id = "PB_SeedProbe";
            picker.args.initial_query.limit = picker.adapter.page_size;
            picker.args.initial_query.order = PageOrder::Desc;
            init = true;
        }
        picker.open = ImGui::IsPopupOpen("PB_SeedProbe");
        picker.Draw([&](const soasim::ui::PickResult& pr, const std::optional<RowT>& row) {
            if (pr.ok && row) {
                // Autofill from the selected group row
                seed_probe_id = row->id; // when codec refactor lands, write seed_probe_id instead
                inst().ini_dirty_ = true;
                inst().unique_seed_count_ = -1;
                FutureQueue::Enqueue(
                    DeltaSeedRepo::ListUniqueForProbeAsync(row->id),
                    // on success
                    [](DbResult<std::vector<DeltaSeedRow>> rows)
                    {
                        if (rows.ok) inst().unique_seed_count_ = rows.value.size();
                        else GuiToastBus::Error("Unable to get Unique seed count", rows.error.message);
                    },
                    // on error
                    [](std::exception_ptr)
                    {
                        GuiToastBus::Error("Error getting Unique seed count");
                    }
                );
            }
            });
    }

    if (ImGui::Button("Pick ExplorerSettings...")) {
        ImGui::OpenPopup("PB_ExplorerSettings");
    }

    {
        using RowT = simcore::db::ExplorerSettingsLite; // or: simcore::db::BattleRunGroupRow
        static soasim::ui::LedgerPicker<RowT> picker;
        static bool init = false;
        if (!init) {
            picker.adapter = soasim::ui::adapters::MakeExplorerSettingsAdapter(/*page_size*/100);
            picker.args.modal_id = "PB_ExplorerSettings";
            picker.args.initial_query.limit = picker.adapter.page_size;
            picker.args.initial_query.order = PageOrder::Desc;
            init = true;
        }
        picker.open = ImGui::IsPopupOpen("PB_ExplorerSettings");
        picker.Draw([&](const soasim::ui::PickResult& pr, const std::optional<RowT>& row) {
            if (pr.ok && row) {
                // Autofill from the selected group row
                settings_id = row->id;
                inst().ini_dirty_ = true;
                inst().battle_plan_count_ = -1;
                FutureQueue::Enqueue(
                    ExplorerSettingsPlanLinkRepo::GetPlanCountAsync(row->id),
                    // on success
                    [](DbResult<int> count)
                    {
                        if (count.ok) inst().battle_plan_count_ = count.value;
                        else GuiToastBus::Error("Unable to get BattlePlan count", count.error.message);
                    },
                    // on error
                    [](std::exception_ptr)
                    {
                        GuiToastBus::Error("Error getting BattlePlan count");
                    }
                );
            }
            });
    }

    ImGui::Text("settings_id: %lld | ", settings_id); ImGui::SameLine();
    ImGui::Text("seed_probe_id: %lld", seed_probe_id); ImGui::SameLine();

    if (ImGui::InputInt("priority", &priority)) { inst().ini_dirty_ = true; }
    if (ImGui::InputInt("run_ms", &run_ms)) { inst().ini_dirty_ = true; }
    if (ImGui::InputInt("vi_stall_ms", &vi_ms)) { inst().ini_dirty_ = true; }
    if (ImGui::Checkbox("progress_enable", &progress_enable)) { inst().ini_dirty_ = true; }

    if (inst().ini_dirty_) {
        bp.settings_id = settings_id;
        bp.seed_probe_id = seed_probe_id;
        bp.priority = priority;
        bp.run_ms = (uint32_t)run_ms;
        bp.vi_stall_ms = (uint32_t)vi_ms;
        bp.progress_enable = progress_enable;
        bp.set_section(*inst().ini_);
    }
}

void PhaseBuilderPane::drawValidation() {
    inst().validation_errors_.clear();
    if (!inst().ini_ || inst().selected_kind_idx_ < 0 || inst().selected_kind_idx_ >= (int)inst().kinds_.size()) return;

    const int pk = inst().kinds_[inst().selected_kind_idx_].id;
    auto errs = simcore::db::phasebuilder::PhaseBuilderService::Validate(pk, *inst().ini_);
    for (auto& e : errs) inst().validation_errors_.push_back({ e.field, e.message });

    if (!inst().validation_errors_.empty()) {
        ImGui::SeparatorText("Validation");
        for (auto& e : inst().validation_errors_) {
            ImGui::BulletText("%s: %s", e.first.c_str(), e.second.c_str());
        }
    }
}

void PhaseBuilderPane::startPreviewAsync() {
    if (!inst().ini_ || inst().selected_kind_idx_ < 0 || inst().selected_kind_idx_ >= (int)inst().kinds_.size()) return;
    inst().preview_state_ = PreviewState::Working;
    inst().preview_.reset();
    inst().preview_err_.clear();
    const int pk = inst().kinds_[inst().selected_kind_idx_].id;
    const auto doc = inst().ini_.value();
    inst().preview_future_ = std::async(std::launch::async, [pk, doc]() {
        return simcore::db::phasebuilder::PhaseBuilderService::Preview(pk, doc);
        });
}

void PhaseBuilderPane::drawPreview() {
    if (inst().preview_state_ == PreviewState::Working) {
        if (inst().preview_future_.valid()) {
            auto st = inst().preview_future_.wait_for(std::chrono::seconds(0));
            if (st == std::future_status::ready) {
                auto r = inst().preview_future_.get();
                if (r.ok) {
                    inst().preview_ = std::move(r.value);
                    inst().preview_state_ = PreviewState::Ready;
                }
                else {
                    inst().preview_err_ = r.error.message;
                    inst().preview_state_ = PreviewState::Error;
                }
            }
        }
        ImGui::TextUnformatted("Previewing…");
        return;
    }

    if (inst().preview_state_ == PreviewState::Error) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Preview failed: %s", inst().preview_err_.c_str());
        return;
    }

    if (inst().preview_state_ != PreviewState::Ready || !inst().preview_) return;

    ImGui::SeparatorText("Preview");
    auto& pv = *inst().preview_;
    if (pv.tasmovie) {
        const auto& t = *pv.tasmovie;
        ImGui::Text("TasMovie jobs: %lld", (long long)t.jobs);
        if (t.artifact_exists) ImGui::Text("Artifact OK%s", t.artifact_size ? (", size available") : "");
        for (auto& w : t.warnings) ImGui::BulletText("%s", w.c_str());
    }
    if (pv.seedprobe) {
        const auto& s = *pv.seedprobe;
        ImGui::Text("Neutral jobs: %lld", (long long)s.neutral_jobs);
        ImGui::Text("Grid jobs: %lld", (long long)s.grid_jobs);
        if (s.unique_deferred) {
            ImGui::TextDisabled("Unique jobs will be scheduled after Grid; count depends on discovered deltas.");
        }
        else if (s.unique_jobs.has_value()) {
            ImGui::Text("Unique jobs: %lld", (long long)*s.unique_jobs);
        }
        for (auto& w : s.warnings) ImGui::BulletText("%s", w.c_str());
    }
    if (pv.explorer) {
        auto& e = *pv.explorer;
        if (inst().battle_plan_count_ > 0 && inst().unique_seed_count_ > 0) 
        {
            ImGui::Text("Jobs: %lld", inst().battle_plan_count_ * inst().unique_seed_count_);
            e.jobs = inst().battle_plan_count_ * inst().unique_seed_count_;
        }
        else ImGui::Text("Jobs: calcluating...");
        
        ImGui::Text("Predicates: %d", (int)e.predicate_count);
        for (auto& w : e.warnings) ImGui::BulletText("%s", w.c_str());
    }
}

void PhaseBuilderPane::startSubmitAsync() {
    if (!inst().ini_ || inst().selected_kind_idx_ < 0 || inst().selected_kind_idx_ >= (int)inst().kinds_.size()) return;
    inst().submit_state_ = SubmitState::Working;
    inst().submit_err_.clear();
    inst().created_job_set_id_.reset();

    const int pk = inst().kinds_[inst().selected_kind_idx_].id;
    auto ini_sorted = inst().ini_->to_string_sorted();
    auto purpose = inst().purpose_;
    auto meta = inst().meta_text_;
    auto preview_copy = inst().preview_;

    inst().submit_future_ = std::async(std::launch::async, [pk, ini_sorted, purpose, meta, preview_copy]() -> DbResult<std::pair<int64_t, int>> {
        auto jsf = simcore::db::DataService::CreateJobSetAsync(purpose.empty() ? std::optional<std::string>{} : std::optional<std::string>{ purpose }, pk, {}, {}, {}, meta, {}, {});
        auto jsr = jsf.get();
        if (!jsr.ok) return DbResult<std::pair<int64_t, int>>::Err(jsr.error);
        int64_t job_set_id = jsr.value;

        auto encf = simcore::db::DataService::EncodeJobSetWithCodecAsync(pk, job_set_id, ini_sorted, {});
        auto encr = encf.get();
        if (!encr.ok) return DbResult<std::pair<int64_t, int>>::Err(encr.error);

        if (preview_copy && (pk == simcore::PK_TasMovie || pk == simcore::PK_BattleTurnRunner)) {
            int64_t expected = 0;
            if (pk == simcore::PK_TasMovie && preview_copy->tasmovie) expected = preview_copy->tasmovie->jobs;
            if (pk == simcore::PK_BattleTurnRunner && preview_copy->explorer) expected = preview_copy->explorer->jobs;
            auto setf = simcore::db::DataService::SetJobSetExpectedTotalAsync(job_set_id, expected, {});
            auto setr = setf.get();
            if (!setr.ok) return DbResult<std::pair<int64_t, int>>::Err(setr.error);
        }

        return DbResult<std::pair<int64_t, int>>::Ok({ job_set_id, pk });
        });
}

void PhaseBuilderPane::drawSubmit() {
    if (inst().submit_state_ == SubmitState::Working) {
        if (inst().submit_future_.valid()) {
            auto st = inst().submit_future_.wait_for(std::chrono::seconds(0));
            if (st == std::future_status::ready) {
                auto r = inst().submit_future_.get();
                if (r.ok) {
                    inst().created_job_set_id_ = r.value.first;
                    inst().submit_state_ = SubmitState::Done;
                }
                else {
                    inst().submit_err_ = r.error.message;
                    inst().submit_state_ = SubmitState::Error;
                }
            }
        }
        ImGui::TextUnformatted("Submitting…");
        return;
    }

    if (inst().submit_state_ == SubmitState::Error) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Submit failed: %s", inst().submit_err_.c_str());
    }

    if (inst().submit_state_ == SubmitState::Done && inst().created_job_set_id_) {
        ImGui::Text("Created Job Set ID: %lld", (long long)*inst().created_job_set_id_);
    }

    bool can_submit = inst().ini_.has_value() && inst().validation_errors_.empty() && inst().selected_kind_idx_ >= 0 && inst().selected_kind_idx_ < (int)inst().kinds_.size();
    ImGui::InputText("Purpose", &inst().purpose_);
    static std::string meta_buf;
    if (!inst().meta_text_) meta_buf.clear();
    if (ImGui::InputTextMultiline("Meta", &meta_buf)) { inst().meta_text_ = meta_buf.empty() ? std::optional<std::string>{} : std::optional<std::string>{ meta_buf }; }
    if (ImGui::Button("Create Job Set & Enqueue") && can_submit) {
        startSubmitAsync();
    }
}

void PhaseBuilderPane::Draw() {
    ImGui::Begin("Phase Builder");

    drawKindPicker();
    if (inst().selected_kind_idx_ >= 0 && inst().selected_kind_idx_ < (int)inst().kinds_.size()) {
        const int pk = inst().kinds_[inst().selected_kind_idx_].id;

        if (ImGui::Button("Reset to Defaults")) {
            inst().ini_.reset();
            ensureDefaults();
        }
        ImGui::SameLine();
        if (ImGui::Button("Preview")) startPreviewAsync();
        ImGui::SameLine();
        if (ImGui::Button("Validate")) { /* just redraw triggers validation */ }

        switch (pk) {
        case simcore::PK_TasMovie:         drawTasMovieForm();  break;
        case simcore::PK_SeedProbe:        drawSeedProbeForm(); break;
        case simcore::PK_BattleTurnRunner: drawExplorerRunForm(); break;
        default: break;
        }

        drawValidation();
        drawPreview();
        ImGui::Separator();
        drawSubmit();

        ImGui::SeparatorText("Blueprint (INI)");
        if (inst().ini_) {
            std::vector<std::string> ini_lines = inst().ini_->to_string_lines_preserve_order();
            ImGui::BeginChild("##ini", ImVec2(0, 0), true);
            for (const auto& e : ini_lines) {
                ImGui::Text("%s", e.c_str());
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
}
