#include "BattleRunSettingsPane.h"
#include "../Popups/IdRepoAdapters.h"
#include "Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../Widgets/BattleContextTree.h"
#include "DB/ProgramDB/BattleContextDBCodec.h"

#include <chrono>

using simcore::db::DbResult;
using simcore::db::RetryPolicy;

namespace soasim::ui {

    static constexpr const char* UIConfigSectionName = "ui_config";
    static constexpr const char* PredicateSectionName = "predicate";
    static constexpr int SearchEntryWidth = 150;
    static constexpr int FakeAtkInputWidth = 150;

    void BattleRunSettingsPane::Draw() {
        ImGui::Begin("Battle Run Settings");

        const float w = ImGui::GetContentRegionAvail().x;
        const float col_w = w / 3.0f;

        ImGui::BeginChild("Left", ImVec2(col_w, 0), true);
        draw_left_library_();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("Middle", ImVec2(col_w, 0), true);
        draw_middle_();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("Right", ImVec2(0, 0), true);
        draw_right_();
        ImGui::EndChild();

        ImGui::End();
    }

    // ----------------------------- LEFT -----------------------------

    void BattleRunSettingsPane::draw_left_library_() {
        draw_left_ui_actions_();
        ImGui::Spacing();
        draw_left_predicates_();
        ImGui::Spacing();
        draw_left_templates_();
    }

    void BattleRunSettingsPane::draw_left_ui_actions_() {
        ImGui::SeparatorText("UI Actions");
        ImGui::SetNextItemWidth(SearchEntryWidth);
        ImGui::InputTextWithHint("##ua_s", "search preset...", &search_ui_actions_);
        ImGui::SameLine();
        if (ImGui::Button("Refresh##ua")) {
            auto fut = simcore::db::DataService::ListActionPresetsAsync(search_ui_actions_, 100);
            if (fut.valid()) {
                auto r = fut.get();
                if (r.ok) ui_action_results_ = std::move(r.value);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("New Preset")) {
            ui_preset_popup_.OpenNew();
        }

        ImGui::BeginChild("ua_list", ImVec2(0, 220), true);
        for (const auto& row : ui_action_results_) {
            ImGui::Selectable(row.name.c_str(), false);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(BRB_PAYLOAD_UI_ACTION_PRESET, &row.id, sizeof(int64_t));
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::EndChild();

        {
            auto [saved, new_id] = ui_preset_popup_.Draw();
            if (saved) {
                GuiToastBus::Info("UI Action Preset saved");
                // Optional: refresh the list so the new preset appears
                auto fut = simcore::db::DataService::ListActionPresetsAsync(search_ui_actions_, 100);
                if (fut.valid()) {
                    auto r = fut.get();
                    if (r.ok) ui_action_results_ = std::move(r.value);
                }
            }
        }
    }

    void BattleRunSettingsPane::draw_left_predicates_() {
        ImGui::SeparatorText("Predicates");
        ImGui::SetNextItemWidth(SearchEntryWidth);
        ImGui::InputTextWithHint("##pp_s", "search predicate...", &search_predicates_);
        ImGui::SameLine();
        if (ImGui::Button("Refresh##pp")) {
            auto fut = simcore::db::DataService::ListPredicateSpecsAsync(search_predicates_, 100);
            if (fut.valid()) {
                auto r = fut.get();
                if (r.ok) predicate_results_ = std::move(r.value);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("New Predicate")) {
            pred_popup_.OpenNew();
            ImGui::OpenPopup("NewPredicateSpecPopup");
        }

        ImGui::BeginChild("pp_list", ImVec2(0, 220), true);
        for (const auto& row : predicate_results_) {
            ImGui::Selectable(row.description.c_str(), false);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(BRB_PAYLOAD_PREDICATE_SPEC, &row.id, sizeof(int64_t));
                ImGui::TextUnformatted(row.description.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::EndChild();

        {
            auto [saved, new_id] = pred_popup_.Draw();
            if (saved) {
                GuiToastBus::Info("Predicate saved");
                // Optional: refresh the list so the new predicate appears
                auto fut = simcore::db::DataService::ListPredicateSpecsAsync(search_predicates_, 100);
                if (fut.valid()) {
                    auto r = fut.get();
                    if (r.ok) predicate_results_ = std::move(r.value);
                }
            }
        }
    }

    void BattleRunSettingsPane::draw_left_templates_() {
        ImGui::SeparatorText("Authoring Templates");
        ImGui::SetNextItemWidth(SearchEntryWidth);
        ImGui::InputTextWithHint("##tpl_s", "search template...", &search_templates_);
        ImGui::SameLine();
        if (ImGui::Button("Refresh##tpl")) {
            auto fut = simcore::db::AuthoringTemplatesRepo::ListLiteAsync(search_templates_, 100);
            if (fut.valid()) {
                auto r = fut.get();
                if (r.ok) template_results_ = std::move(r.value);
            }
        }

        ImGui::BeginChild("tpl_list", ImVec2(0, 180), true);
        for (const auto& row : template_results_) {
            if (ImGui::Selectable(row.name.c_str(), false)) {
                load_authoring_template_(row.id);
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(BRB_PAYLOAD_TEMPLATE, &row.id, sizeof(int64_t));
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::EndChild();
    }

    // ----------------------------- MIDDLE -----------------------------

    void BattleRunSettingsPane::draw_middle_() {
        ImGui::SeparatorText("UI Config");
        draw_ui_config_editor_();
        ImGui::Spacing();
        ImGui::SeparatorText("Predicates");
        draw_predicates_editor_();
    }

    void BattleRunSettingsPane::add_turn_() {
        const int n = (party_size_ > 0 ? party_size_ : 4);
        std::vector<UiActionInstance> turn{};
        for (int s = 0; s < n; ++s) turn.emplace_back(s, 0);
        ui_config_.actions.push_back(turn);
    }

    UiActionInstance* BattleRunSettingsPane::find_instance_(int t, int s) {
        if (0 <= t < ui_config_.actions.size()) {
            for (auto& a : ui_config_.actions.at(t)) if (a.actor_slot == s) return &a;
        }
        return nullptr;
    }

    const UiActionInstance* BattleRunSettingsPane::find_instance_(int t, int s) const {
        if (0 <= t < ui_config_.actions.size()) {
            for (const auto& a : ui_config_.actions.at(t)) if (a.actor_slot == s) return &a;
        }
        return nullptr;
    }

    bool BattleRunSettingsPane::all_slots_filled_() const {
        if (party_size_ <= 0) return false;
        if (ui_config_.actions.empty()) return false;
        for (const auto& t : ui_config_.actions)
            for (const auto& a : t) if (a.preset_id <= 0) return false;
        return true;
    }

    void BattleRunSettingsPane::draw_ui_config_editor_() {
        ImGui::SetNextItemWidth(FakeAtkInputWidth);
        ImGui::InputInt("Fake Attack Budget", &ui_config_.fake_attack_budget);
        ImGui::SameLine();
        if (ImGui::Button("Add Turn")) add_turn_();

        ImGui::BeginChild("turns", ImVec2(0, 260), true);

        for (int t = 0; t < ui_config_.actions.size(); t++) {
            ImGui::PushID(t);
            ImGui::Separator();
            ImGui::Text("Turn %d", t+1);
            ImGui::SameLine();
            if (ImGui::Button("Delete##t")) {
                ui_config_.actions.erase(ui_config_.actions.begin() + t);
                ImGui::PopID();
                continue;
            }
            auto turn = ui_config_.actions[t];

            // Slots row

            int turn_height = 25 * party_size_;
            ImGui::BeginChild("row", ImVec2(0, turn_height), false);
            for (int s = 0; s < turn.size(); ++s) {
                ImGui::PushID(s);
                ImGui::BeginGroup();
                auto inst = turn[s];

                std::string label;
                if (inst.preset_id > 0) {
                    label = "Slot " + std::to_string(s) + " : #" + std::to_string(inst.preset_id);
                }
                else {
                    label = "Slot " + std::to_string(s) + " : [empty]";
                }

                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 20))) {

                    // open New/Edit UI action here
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(BRB_PAYLOAD_UI_ACTION_PRESET)) {
                        int64_t preset_id = *(const int64_t*)p->Data;
                        inst.preset_id = preset_id;
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::SameLine();
                if (inst.preset_id > 0) if (ImGui::SmallButton("X")) inst.preset_id = 0;

                ImGui::EndGroup();
                ImGui::PopID();
            }
            //ImGui::NewLine();
            ImGui::EndChild();

            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void BattleRunSettingsPane::draw_predicates_editor_() {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(BRB_PAYLOAD_PREDICATE_SPEC)) {
                int64_t id = *(const int64_t*)p->Data;
                predicates_.push_back({ id });
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::BeginChild("pred_list", ImVec2(0, 180), true);
        for (int i = 0; i < (int)predicates_.size(); ++i) {
            ImGui::PushID(i);
            ImGui::Text("Predicate #%lld", (long long)predicates_[i].predicate_id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Up") && i > 0) { std::swap(predicates_[i], predicates_[i - 1]); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Down") && i + 1 < (int)predicates_.size()) { std::swap(predicates_[i], predicates_[i + 1]); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) { predicates_.erase(predicates_.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ----------------------------- RIGHT -----------------------------

    void BattleRunSettingsPane::draw_right_() {
        draw_context_loader_();
        ImGui::Spacing();
        draw_counts_and_estimate_();
        ImGui::Spacing();
        draw_save_actions_();
    }

    void BattleRunSettingsPane::on_context_row_found(int64_t artifact_id, int version) {
        FutureQueue::Enqueue(
            simcore::db::ObjectStore::GetTextAsync(artifact_id),
            // on success
            [this, version](DbResult<std::string> r) {
                if (r.ok) {
                    has_context_ = true;
                    soa::battle::ctx::codec::decode(r.value, bc_);
                    bc_version_ = version;
                    reconcile_party_size_();
                    context_status_.store(BattleContextRequestState::Decoded);
                    GuiToastBus::Info("BattleContext loaded");
                }
                else {
                    has_context_ = false;
                    GuiToastBus::Error("Failed to load BattleContext");
                }
            },
            // on failure
            [this](std::exception_ptr ep) {
                has_context_ = false;
                GuiToastBus::Error("Failed to load BattleContext");
            }
        );
    }

    void BattleRunSettingsPane::on_savestate_row_found(int64_t savestate_id) {
        savestate_id_ = savestate_id;
        FutureQueue::Enqueue(
            simcore::db::DataService::GetLatestBattleContextForSavestateAsync(savestate_id),
            // on success
            [this](DbResult<std::optional<BattleContextRow>> r) {
                if (r.ok) {
                    if (r.value.has_value())
                        on_context_row_found(r.value.value().artifact_id, r.value.value().codec_version);
                    else {
                        bc_version_ = 0;
                        GuiToastBus::Info("No BattleContext for this savestate");
                    }
                }
                else {
                    has_context_ = false;
                    GuiToastBus::Error("Failed to load BattleContext");
                }
            },
            // on failure
            [this](std::exception_ptr ep) {
                has_context_ = false;
                GuiToastBus::Error("Failed to load BattleContext");
            }
        );
    }

    void BattleRunSettingsPane::on_seed_probe_picked_(int64_t spid) {
        seed_probe_id_ = spid;
        FutureQueue::Enqueue<DbResult<int64_t>>(
            simcore::db::DataService::GetSavestateForSeedProbeAsync(spid),
            // on success
            [this](DbResult<int64_t> r) {
                if (r.ok) {
                    on_savestate_row_found(r.value);
                }
                else {
                    has_context_ = false;
                    GuiToastBus::Error("Failed to load BattleContext");
                }
            },
            // on failure
            [this](std::exception_ptr ep) {
                has_context_ = false;
                GuiToastBus::Error("Failed to load BattleContext"); 
            }
        );
    }

    void BattleRunSettingsPane::reconcile_party_size_() {
        party_size_ = 0;
        for (auto s : bc_.slots) if (s.is_alive && s.is_player) party_size_++;

        if (party_size_ <= 0) return;
        for (auto& t : ui_config_.actions) {
            if (t.size() > party_size_) for (int a = party_size_; a < t.size(); a++) t.pop_back();
            else if (t.size() < party_size_) for (int a = t.size(); a < party_size_; a++) t.emplace_back(a, 0);
        }
    }

    void BattleRunSettingsPane::wait_for_context(int64_t job_id, uint32_t poll_rate_ms) {
        bool requesting_context = true;
        while (requesting_context) {
            auto jb = JobsRepo::Get(job_id);
            if (!jb.ok) {
                requesting_context = false;
                context_status_.store(BattleContextRequestState::None);
                GuiToastBus::Error("Unable to find BattleContext job");
                return;
            }

            if (jb.value.state == "RUNNING") {
                context_status_.store(BattleContextRequestState::Running);
            }
            else if (jb.value.state == "SUCCEEDED") {
                context_status_.store(BattleContextRequestState::Success);
                requesting_context = false;
            }
            else if (jb.value.state == "FAILED") {
                context_status_.store(BattleContextRequestState::Failure);
                requesting_context = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(poll_rate_ms));
        }
    }

    void BattleRunSettingsPane::draw_context_loader_() {
        ImGui::SeparatorText("Battle Context");
        ImGui::Text("Party size: %d", party_size_ > 0 ? party_size_ : 4);
        
        ImGui::BeginChild("##ContextTree", ImVec2(0, 200));
        if (has_context_)
        {
            ImGui::SetNextItemOpen(true);
            BattleContextTree::DrawTree(bc_);
        }
        else if (context_status_ != BattleContextRequestState::None && context_status_ != BattleContextRequestState::Decoded) {
            ImGui::Text(GetBCStateString(context_status_.load()).c_str());
            if (context_status_ == BattleContextRequestState::Success) {
                on_savestate_row_found(savestate_id_);
            }
        }
        else {
            ImGui::Text("No Context Loaded.");
        }
        ImGui::EndChild();

        if (seed_probe_id_ > 0) ImGui::Text("Seed Probe: %lld", (long long)seed_probe_id_);
        if (savestate_id_ > 0) ImGui::Text("Savestate:  %lld", (long long)savestate_id_);

        if (ImGui::Button("Pick Seed Probe...")) ImGui::OpenPopup("BRS_SeedProbePicker");

        {
            using RowT = simcore::db::SeedProbeLite; // or: simcore::db::BattleRunGroupRow
            static soasim::ui::LedgerPicker<RowT> picker;
            static bool init = false;
            if (!init) {
                picker.adapter = soasim::ui::adapters::MakeSeedProbeAdapter();
                picker.args.modal_id = "BRS_SeedProbePicker";
                picker.args.initial_query.limit = picker.adapter.page_size;
                picker.args.initial_query.order = PageOrder::Desc;
                init = true;
            }
            picker.open = ImGui::IsPopupOpen("BRS_SeedProbePicker");
            picker.Draw([&](const soasim::ui::PickResult& pr, const std::optional<RowT>& row) {
                if (pr.ok && row) {
                    on_seed_probe_picked_(row.value().id);
                }
                });
        }

        std::string ctx_btn_text = bc_version_ == 0 ? "Get Context" : "Get Context Update";
        if (seed_probe_id_ > 0 && savestate_id_ > 0 &&bc_version_ < soa::battle::ctx::codec::ver)
        {
            if (ImGui::Button(ctx_btn_text.c_str())) {
                simcore::db::battle::ctx::BlueprintIni bp{
                    .savestate_id = savestate_id_,
                    .priority = 100,
                    .run_ms = 20000,
                    .vi_stall_ms = 20000
                };
                context_status_.store(BattleContextRequestState::None);
                FutureQueue::Enqueue<DbResult<int64_t>>(
                    DataService::GetNewBattleContextAsync(bp.to_string()),
                    // on success
                    [this](DbResult<int64_t> job_id) {
                        if (job_id.ok) {
                            context_status_.store(BattleContextRequestState::Queued);
                            std::thread(&BattleRunSettingsPane::wait_for_context, this, job_id.value, /*poll*/ 500).detach();
                        }
                        else {
                            GuiToastBus::Error("Failed to get new BattleContext");
                        }
                    },
                    // on error
                    [](std::exception_ptr ep) {
                        GuiToastBus::Error("Failed to get new BattleContext");
                    }
                )
                ;
            }
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(BRB_PAYLOAD_TEMPLATE)) {
                int64_t tid = *(const int64_t*)p->Data;
                load_authoring_template_(tid);
            }
            ImGui::EndDragDropTarget();
        }
    }

    void BattleRunSettingsPane::draw_counts_and_estimate_() {
        ImGui::SeparatorText("Counts");
        int turns = ui_config_.actions.size();
        int filled = 0; 
        for (const auto& t : ui_config_.actions) for (const auto& a : t) if (a.preset_id > 0) ++filled;

        ImGui::Text("Turns: %d", turns);
        ImGui::Text("UI Actions filled: %d", filled);
        ImGui::Text("Predicates: %d", (int)predicates_.size());

        if (has_context_) {
            simcore::battleexplorer::UI_Config cfg{};
            if (build_ui_config_from_draft_(cfg)) {
                simcore::battleexplorer::BattleExplorer be{""};
                uint64_t base = be.estimate_paths_no_fake(cfg, bc_);
                uint64_t with_fake = be.estimate_paths_with_fake(cfg, base);
                ImGui::Text("Base plans: %llu", (unsigned long long)base);
                ImGui::Text("With fake budget: %llu", (unsigned long long)with_fake);
            }
            else {
                ImGui::TextDisabled("Estimate unavailable (draft -> UI_Config conversion failed)");
            }
        }
        else {
            ImGui::TextDisabled("Load BattleContext to estimate paths");
        }
    }

    void BattleRunSettingsPane::draw_save_actions_() {
        ImGui::SeparatorText("Save / Materialize");

        bool can_save = has_context_ && all_slots_filled_();
        ImGui::BeginDisabled(!can_save);

        ImGui::InputText("Settings Name", &settings_name_);
        ImGui::InputTextMultiline("Settings Description", &settings_desc_, ImVec2(0, 60));
        const bool name_ok = !settings_name_.empty();
        const bool desc_ok = !settings_desc_.empty();

        if (ImGui::Button("Save Explorer Settings") && name_ok && desc_ok) {
            simcore::battleexplorer::UI_Config cfg{};
            if (!build_ui_config_from_draft_(cfg)) {
                GuiToastBus::Error("Failed to build UI_Config from draft");
                ImGui::EndDisabled();
                return;
            }
            
            std::vector<PredicateSpecRow> preds{};
            for (auto p : predicates_)
            {
                auto pred = PredicateSpecRepo::Get(p.predicate_id);
                if (pred.ok) preds.push_back(pred.value);
                else {
                    GuiToastBus::Error("Failed to obtain row for predicate=" + std::to_string(p.predicate_id));
                    ImGui::EndDisabled();
                    return;
                }
            }

            simcore::phases::AuthoringPayload ap{
                .ui = cfg,
                .predicates = preds,
                .fake_attack_budget = ui_config_.fake_attack_budget,
                .settings_name = settings_name_,
                .settings_description = settings_desc_
            };

            auto rr = simcore::phases::BRSettingsWriter::EnsureSettingsWithPredicatesAndPlans(savestate_id_, bc_, ap);
            if (rr.ok) {
                GuiToastBus::Info("ExplorerSettings saved");
            }
            else {
                GuiToastBus::Error("Failed to save ExplorerSettings");
            }
        }

        ImGui::InputText("Group Name", &run_group_name_);
        ImGui::InputTextMultiline("Group Description", &run_group_desc_, ImVec2(0, 60));
        const bool run_name_ok = !run_group_name_.empty();
        const bool run_desc_ok = !run_group_desc_.empty();

        if (ImGui::Button("Create Battle Run Group") && name_ok && desc_ok && run_name_ok && run_desc_ok && seed_probe_id_ > 0) {
            simcore::battleexplorer::UI_Config cfg{};
            if (!build_ui_config_from_draft_(cfg)) {
                GuiToastBus::Error("Failed to build UI_Config from draft");
                ImGui::EndDisabled();
                return;
            }

            std::vector<PredicateSpecRow> preds{};
            for (auto p : predicates_)
            {
                auto pred = PredicateSpecRepo::Get(p.predicate_id);
                if (pred.ok) preds.push_back(pred.value);
                else {
                    GuiToastBus::Error("Failed to obtain row for predicate=" + std::to_string(p.predicate_id));
                    ImGui::EndDisabled();
                    return;
                }
            }

            simcore::phases::AuthoringPayload ap{
                .ui = cfg,
                .predicates = preds,
                .fake_attack_budget = ui_config_.fake_attack_budget,
                .settings_name = settings_name_,
                .settings_description = settings_desc_
            };

            auto rs = simcore::phases::BRSettingsWriter::EnsureSettingsWithPredicatesAndPlans(savestate_id_, bc_, ap);
            if (rs.ok) {
                auto settings_id = rs.value;
                auto rg = simcore::phases::BRSettingsWriter::CreateRunGroup(seed_probe_id_, savestate_id_, settings_id, run_group_name_, run_group_desc_);
                if (rg.ok) GuiToastBus::Info("BattleRunGroup created");
                else GuiToastBus::Error("Failed to create BattleRunGroup");
            }
            else {
                GuiToastBus::Error("Failed to save settings for group");
            }
        }

        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::Button("Save Authoring Template...")) {
            save_authoring_template_();
        }
    }

    void BattleRunSettingsPane::save_authoring_template_() {
        if (ui_config_.actions.empty()) {
            GuiToastBus::Warn("Nothing to save");
            return;
        }
        std::vector<simcore::db::UiConfigRow> rows; rows.reserve(ui_config_.actions.size());
        for (int t = 0; t < ui_config_.actions.size(); t++)
        {
            std::vector<UiActionInstance> turn = ui_config_.actions[t];
            for (const auto& a : turn) {
                simcore::db::UiConfigRow r{};
                r.preset_id = a.preset_id;
                r.turn_index = t;
                r.actor_slot = a.actor_slot;
                rows.push_back(r);
            }
        }
        auto ins = simcore::db::DataService::InsertUiConfigRowsAsync(rows).get();
        if (!ins.ok) { GuiToastBus::Error("Failed to persist ui_config_rows"); return; }

        std::vector<std::string> ids{};
        ids.reserve(ins.value.size());
        for (auto id : ins.value)
            ids.emplace_back(std::to_string(id));

        IniDoc uiini{};
        uiini.ensure_section(UIConfigSectionName);
        uiini.set_list(UIConfigSectionName, "ids", ids);
        uiini.set(UIConfigSectionName, "budget", std::to_string(ui_config_.fake_attack_budget));

        std::vector<std::string> ps{};
        ps.reserve(predicates_.size());
        for (auto p : predicates_)
            ps.emplace_back(std::to_string(p.predicate_id));

        IniDoc pini{};
        pini.ensure_section(PredicateSectionName);
        pini.set_list(PredicateSectionName, "ids", ps);

        simcore::db::AuthoringTemplateRow tr{};
        tr.name = "New Template";
        tr.description = "Draft snapshot";
        tr.ui_config_ini = std::move(uiini.to_string_sorted());
        tr.predicate_specs_ini = std::move(pini.to_string_sorted());

        auto sr = simcore::db::AuthoringTemplatesRepo::Insert(tr);
        if (sr.ok) GuiToastBus::Info("Template saved");
        else GuiToastBus::Error("Failed to save template");
    }

    void BattleRunSettingsPane::load_authoring_template_(int64_t template_id) {
        auto r = simcore::db::AuthoringTemplatesRepo::Get(template_id);
        if (!r.ok) { GuiToastBus::Error("Failed to load template"); return; }
        auto row = r.value;

        uint32_t budget = 0;
        std::vector<int64_t> ui_ids;
        {
            const auto& ini = IniDoc::parse(row.ui_config_ini);

            auto id_strings = ini.get_list(UIConfigSectionName, "ids");
            for (auto id_s : id_strings) {
                size_t pos = 0;
                long long v = std::stoll(id_s, &pos, 0);
                ui_ids.emplace_back(static_cast<int64_t>(v));
            }

            budget = ini.get_u32(UIConfigSectionName, "budget");
        }

        std::vector<int64_t> pred_ids;
        {
            const auto& ini = IniDoc::parse(row.predicate_specs_ini);

            auto p_strings = ini.get_list(UIConfigSectionName, "ids");
            for (auto p_s : p_strings) {
                size_t pos = 0;
                long long v = std::stoll(p_s, &pos, 0);
                pred_ids.emplace_back(static_cast<int64_t>(v));
            }
        }

        ui_config_.actions.clear();
        ui_config_.fake_attack_budget = budget;

        auto rr = simcore::db::DataService::GetUiConfigRowsByIdsAsync(ui_ids).get();
        if (rr.ok) {
            for (auto& rc : rr.value) {
                if (ui_config_.actions.size() <= rc.turn_index)
                    for (int t = ui_config_.actions.size(); t < rc.turn_index; t++) 
                        add_turn_();

                ui_config_.actions[rc.turn_index][rc.actor_slot].actor_slot = rc.actor_slot;
                ui_config_.actions[rc.turn_index][rc.actor_slot].preset_id = rc.preset_id;
            }
        }
        else {
            GuiToastBus::Warn("Some ui_config_row ids missing; they were skipped");
        }

        predicates_.clear();
        for (auto id : pred_ids) predicates_.push_back({ id });
    }

    // ----------------------------- conversion -----------------------------

    bool BattleRunSettingsPane::build_ui_config_from_draft_(/*out*/simcore::battleexplorer::UI_Config& cfg) const {
        // Build per-turn, per-slot actions using preset rows.
        // Minimal construction using TurnActionPresetRepo to expand to UI actions.
        // Assumes cfg has: fake_attack_budget, turns vector; each turn has per-slot action entries.

        for (const auto& t : ui_config_.actions)
        {
            simcore::battleexplorer::UI_Turn turn{};
            for (const auto& a : t) {

                if (a.preset_id <= 0) return false;
                auto pr = simcore::db::TurnActionPresetRepo::Get(a.preset_id);
                if (!pr.ok) return false;
                const auto& p = pr.value;

                simcore::battleexplorer::UI_Action ua{};
                ua.macro = (soa::battle::actions::BattleAction)p.macro;

                simcore::battleexplorer::TargetBinding tb{};
                tb.kind = (simcore::battleexplorer::TargetBindingKind)p.target_kind;

                // use this with kind to create mask
                p.target_expr_ini;

                tb.mask = 0;
                tb.var_id = p.same_as_pc;

                ua.target = tb;
                ua.actor_slot = a.actor_slot;

                turn.push_back(ua);
            }
            cfg.turns.push_back(turn);
        }
        return true;
    }

} // namespace soasim::ui
