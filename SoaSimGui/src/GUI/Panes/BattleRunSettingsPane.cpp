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

    void BattleRunSettingsPane::OnActivated() {
        if (inited_) return;
        load_ui_actions();
        load_predicates();
        inited_ = true;
    }

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

        {
            if (show_ui_preset_popup_) {
                if (preset_edit_id_.has_value()) {
                    ui_preset_popup_.Open(preset_edit_id_.value());
                    preset_edit_id_.reset();
                }
                else {
                    ui_preset_popup_.Open();
                }
                show_ui_preset_popup_ = false;
            }
            auto [saved, new_id] = ui_preset_popup_.Draw();
            if (saved) {
                load_ui_actions();
                if (ui_preset_edit_loc_.has_value()) {
                    auto [t, s] = ui_preset_edit_loc_.value();
                    ensure_preset_cached_async_(new_id);
                    ui_config_.actions[t][s].preset_id = new_id;
                    ui_preset_edit_loc_.reset();
                }
            }
        }

        {
            if (show_pred_popup_) {
                if (pred_edit_id_.has_value()) {
                    pred_popup_.Open(pred_edit_id_.value());
                    pred_edit_id_.reset();
                }
                else {
                    pred_popup_.Open();
                }
                show_pred_popup_ = false;
            }
            
            auto [saved, id] = pred_popup_.Draw();
            if (saved) {
                load_predicates();
                if (pred_edit_loc_.has_value()) {
                    auto row = get_pred_row(pred_edit_loc_.value());
                    predicates_[pred_edit_loc_.value()].predicate_id = row.id;
                    predicates_[pred_edit_loc_.value()].name = row.name;
                    predicates_[pred_edit_loc_.value()].description = row.description;
                    pred_edit_loc_.reset();
                }
            }
        }
    }

    // ----------------------------- LEFT -----------------------------

    void BattleRunSettingsPane::load_ui_actions() {
        FutureQueue::Enqueue(
            simcore::db::DataService::ListActionPresetsAsync(search_ui_actions_, 100),
            // on success
            [this](auto r)
            {
                if (r.ok) ui_action_results_ = std::move(r.value);
                else GuiToastBus::Error("Unable to update ui actions");
            },
            // on error
            [](std::exception_ptr e) {
                GuiToastBus::Error("Error on updating ui actions");
            }
        );
    }

    void BattleRunSettingsPane::load_predicates() {
        FutureQueue::Enqueue(
            simcore::db::DataService::ListPredicateSpecsAsync(search_predicates_, 100),
            // on success
            [this](auto r)
            {
                if (r.ok) predicate_results_ = std::move(r.value);
                else GuiToastBus::Error("Unable to update predicates");
            },
            // on error
            [](std::exception_ptr e) {
                GuiToastBus::Error("Error on updating predicates");
            }
        );
    }

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
            load_ui_actions();
        }
        ImGui::SameLine();
        if (ImGui::Button("New Preset")) {
            show_ui_preset_popup_ = true;
        }

        ImGui::BeginChild("ua_list", ImVec2(0, 220), true);
        for (const auto& row : ui_action_results_) {
            std::string name = row.name.empty() ? "[no name]" : row.name;
            std::string rowid = std::format("{}##{}", name, row.id);
            ImGui::Selectable(rowid.c_str(), false);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(BRB_PAYLOAD_UI_ACTION_PRESET, &row.id, sizeof(int64_t));
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::EndChild();


    }

    void BattleRunSettingsPane::draw_left_predicates_() {
        ImGui::SeparatorText("Predicates");
        ImGui::SetNextItemWidth(SearchEntryWidth);
        ImGui::InputTextWithHint("##pp_s", "search predicate...", &search_predicates_);
        ImGui::SameLine();
        if (ImGui::Button("Refresh##pp")) {
            load_predicates();
        }
        ImGui::SameLine();
        if (ImGui::Button("New Predicate")) {
            show_pred_popup_ = true;
        }

        ImGui::BeginChild("pp_list", ImVec2(0, 220), true);
        if (ImGui::BeginTable("pp_tbl", 4, true))
        {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 20);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Abort", ImGuiTableColumnFlags_WidthFixed, 20);
            ImGui::TableSetupColumn("BP", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            for (const auto& row : predicate_results_) {
                ImGui::PushID(row.id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                auto tl = ImGui::GetCursorPos();

                ImGui::Text("%lld", row.id);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", row.name.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", row.abort_on_fail ? "X" : " ");

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0x%08X", row.required_bp.pc);

                ImGui::SetCursorPos(tl);
                const float row_h = ImGui::GetTextLineHeightWithSpacing();
                ImGui::Selectable("##row", 
                    false, 
                    ImGuiSelectableFlags_SpanAllColumns,
                    ImVec2(0, row_h));

                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload(BRB_PAYLOAD_PREDICATE_SPEC, &row.id, sizeof(int64_t));
                    ImGui::TextUnformatted(row.description.c_str());
                    ImGui::EndDragDropSource();
                }

                ImGui::SetItemTooltip("%s", row.description.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

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

                if (inst.preset_id > 0) 
                {
                    if (ImGui::SmallButton("X"))
                    {
                        ui_config_.actions[t][inst.actor_slot].preset_id = 0;
                    }
                    ImGui::SameLine();
                }

                uint32_t k = ((uint32_t)t << 8) | (uint32_t)s;
                bool is_bad = (invalid_cells_.find(k) != invalid_cells_.end());
                if (is_bad) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 70, 70, 255));

                std::string label;
                std::string name = preset_cache_[inst.preset_id].name;
                if (inst.preset_id > 0) {
                    label = "Slot " + std::to_string(s) + " : [" + std::to_string(inst.preset_id) + "] " + name;
                }
                else {
                    label = "Slot " + std::to_string(s) + " : [empty]";
                }

                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 20))) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // Determine context + which cell we’re acting on
                        const int turn_idx = t;                          // current turn index in your loop
                        const int actor_slot = inst.actor_slot;             // or use `s` if that’s your slot variable

                        // If this cell already has a preset, open Edit; otherwise open New
                        show_ui_preset_popup_ = true;
                        if (inst.preset_id > 0) {
                            ui_preset_edit_loc_ = { t, s };
                            preset_edit_id_ = inst.preset_id;
                        }
                    }
                }

                if (is_bad) {
                    ImGui::PopStyleColor();
                    auto it = invalid_reasons_.find(k);
                    if (it != invalid_reasons_.end()) ImGui::SetItemTooltip("%s", it->second.c_str());
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(BRB_PAYLOAD_UI_ACTION_PRESET)) {
                        int64_t preset_id = *(const int64_t*)p->Data;
                        ui_config_.actions[t][inst.actor_slot].preset_id = preset_id;
                        ensure_preset_cached_async_(preset_id);
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::EndGroup();
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    PredicateSpecLite BattleRunSettingsPane::get_pred_row(int64_t id) {
        for (auto p : predicate_results_) {
            if (p.id == id) return p;
        }
    }

    void BattleRunSettingsPane::draw_predicates_editor_() {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(BRB_PAYLOAD_PREDICATE_SPEC)) {
                int64_t id = *(const int64_t*)p->Data;
                auto row = get_pred_row(id);
                predicates_.emplace_back(row.id, row.name, row.description);
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::BeginChild("pred_list", ImVec2(0, 180), true);
        for (int i = 0; i < (int)predicates_.size(); ++i) {
            ImGui::PushID(i);

            if (ImGui::SmallButton("Up") && i > 0) { std::swap(predicates_[i], predicates_[i - 1]); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Down") && i + 1 < (int)predicates_.size()) { std::swap(predicates_[i], predicates_[i + 1]); }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) { predicates_.erase(predicates_.begin() + i); ImGui::PopID(); break; }
            ImGui::SameLine();

            std::string label = std::format("Predicate #{} {}", predicates_[i].predicate_id, predicates_[i].name);

            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 20))) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    show_pred_popup_ = true;
                    pred_edit_loc_ = { i };
                    pred_edit_id_ = predicates_[i].predicate_id;
                }
            }
            ImGui::SetItemTooltip("--%s--\n%s", predicates_[i].name.c_str(), predicates_[i].description.c_str());


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

    void BattleRunSettingsPane::validate_grid_against_context_() {
        invalid_cells_.clear();
        invalid_reasons_.clear();
        if (!has_context_) return;

        // Build present sets
        uint32_t present_enemy_slots_mask = 0;
        for (int s = 4; s <= 11; ++s) if (bc_.slots_[s].present == 1) present_enemy_slots_mask |= (1u << s);

        std::unordered_set<uint16_t> present_items;
        for (int i = 0; i < 80; i++) 
        {
            if (bc_.state.useable_items[i].item_id == 0xFFFF) continue;
            present_items.insert(bc_.state.useable_items[i].item_id);
        }

        for (int t = 0; t < (int)ui_config_.actions.size(); ++t) {
            const auto& turn = ui_config_.actions[t];
            for (const auto& a : turn) {
                if (a.preset_id <= 0) continue;
                auto pr = simcore::db::TurnActionPresetRepo::Get(a.preset_id);
                if (!pr.ok) continue;
                const auto& p = pr.value;

                auto put_bad = [&](const char* msg) {
                    uint32_t k = ((uint32_t)t << 8) | (uint32_t)a.actor_slot;
                    invalid_cells_.insert(k);
                    invalid_reasons_[k] = msg;
                    };

                bool needs_item = (p.macro == (int)soa::battle::actions::BattleAction::UseItem);
                if (needs_item) {
                    if (present_items.find((uint16_t)p.item_id) == present_items.end()) {
                        put_bad("Item not in inventory");
                        continue;
                    }
                }

                if (!p.target_expr_ini.empty()) {
                    IniDoc ini = IniDoc::parse(p.target_expr_ini);
                    if (ini.get("target", "kind", "") == "ByEnemyKind") {
                        int want_id = (int)ini.get_i64("target", "enemy_kind_id", -1);
                        bool any = false;
                        for (int s = 4; s <= 11; ++s) if (bc_.slots_[s].present == 1 && (int)bc_.slots_[s].id == want_id) { any = true; break; }
                        if (!any) { put_bad("Enemy kind not present in this context"); continue; }
                    }
                    continue;
                }

                auto kind = (simcore::battleexplorer::TargetBindingKind)p.target_kind;
                if (kind == simcore::battleexplorer::TargetBindingKind::SingleEnemy) {
                    int s = p.single_slot;
                    if (s < 4 || s > 11 || (present_enemy_slots_mask & (1u << s)) == 0) put_bad("Selected enemy slot is empty");
                }
                else if (kind == simcore::battleexplorer::TargetBindingKind::MultipleEnemies) {
                    uint32_t m = p.mask_bits & present_enemy_slots_mask;
                    if (m == 0) put_bad("No selected slots are present");
                }
                else if (kind == simcore::battleexplorer::TargetBindingKind::SameAsOtherPC) {
                    int s = p.same_as_pc;
                    if (s < 0 || s > 3 || bc_.slots_[s].present != 1) put_bad("Selected PC not present");
                }
            }
        }
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
                    validate_grid_against_context_();
                    ui_preset_popup_.SetContext(has_context_ ? &bc_ : nullptr);
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
        for (auto s : bc_.slots_) if (s.is_alive && s.is_player) party_size_++;

        if (party_size_ <= 0) return;
        for (auto& t : ui_config_.actions) {
            if (t.size() > party_size_) for (int a = party_size_; a <= t.size(); a++) t.pop_back();
            else if (t.size() < party_size_) for (int a = t.size(); a <= party_size_; a++) t.emplace_back(a, 0);
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

        if (ImGui::Button("Pick Seed Probe...")) 
        {
            context_status_ = BattleContextRequestState::None;
            has_context_ = false;
            bc_ = BattleContext();
            ImGui::OpenPopup("BRS_SeedProbePicker");
        }

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
        if (!ins.ok) { GuiToastBus::Error("Failed to persist ui_config_rows", ins.error.message.c_str()); return; }

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

                // params
                if (ua.macro == soa::battle::actions::BattleAction::UseItem) {
                    ua.params.item_id = (uint16_t)p.item_id;
                }

                simcore::battleexplorer::TargetBinding tb{};
                tb.kind = (simcore::battleexplorer::TargetBindingKind)p.target_kind;
                tb.mask = 0;
                tb.var_id = 0xFF;

                // symbolic: ByEnemyKind via INI
                bool handled_expr = false;
                if (!p.target_expr_ini.empty()) {
                    IniDoc ini = IniDoc::parse(p.target_expr_ini);
                    const auto k = ini.get("target", "kind", "");
                    if (k == "ByEnemyKind") {
                        const int want_id = (int)ini.get_i64("target", "enemy_kind_id", -1);
                        const std::string q = ini.get("target", "quantifier", "Any");

                        uint32_t domain_mask = 0;
                        for (int s = 4; s <= 11; ++s) {
                            if (bc_.slots_[s].present == 1 && (int)bc_.slots_[s].id == want_id) {
                                domain_mask |= (1u << s);
                            }
                        }

                        if (domain_mask == 0) {
                            return false; // unresolved symbolic target for this context
                        }

                        if (q == "First") {
                            int first = -1;
                            for (int s = 4; s <= 11 && first < 0; ++s) if (domain_mask & (1u << s)) first = s;
                            tb.kind = simcore::battleexplorer::TargetBindingKind::SingleEnemy;
                            tb.mask = (uint32_t)(1u << first);
                        }
                        else {
                            tb.kind = simcore::battleexplorer::TargetBindingKind::MultipleEnemies;
                            tb.mask = domain_mask; // One-of domain
                        }
                        handled_expr = true;
                    }
                }

                if (!handled_expr) {
                    switch ((simcore::battleexplorer::TargetBindingKind)p.target_kind) {
                    case simcore::battleexplorer::TargetBindingKind::SingleEnemy:
                        tb.mask = (1u << (uint32_t)p.single_slot);
                        break;
                    case simcore::battleexplorer::TargetBindingKind::MultipleEnemies:
                        tb.mask = (uint32_t)p.mask_bits;
                        break;
                    case simcore::battleexplorer::TargetBindingKind::AnyEnemy:
                        // mask unused; domain resolved later
                        break;
                    case simcore::battleexplorer::TargetBindingKind::SameAsOtherPC:
                        tb.var_id = (uint8_t)p.same_as_pc;
                        break;
                    }
                }

                ua.target = tb;
                ua.actor_slot = a.actor_slot;
                turn.push_back(ua);
            }
            cfg.turns.push_back(turn);
        }
        cfg.fakeattack_budget = ui_config_.fake_attack_budget;
        cfg.initial_frames.push_back(GCInputFrame());
        return true;
    }

    void BattleRunSettingsPane::ensure_preset_cached_async_(int64_t id) {
        if (id <= 0) return;

        FutureQueue::Enqueue(
            simcore::db::TurnActionPresetRepo::GetAsync(id),
            [this, id](const DbResult<simcore::db::TurnActionPresetRow>& r) {
                if (r.ok) {
                    preset_cache_.insert_or_assign(id, r.value);
                    GuiToastBus::Success("Preset chached");
                }
                else {
                    GuiToastBus::Error("Failed to load preset", r.error.message);
                }
            },
            [](std::exception_ptr) {
                GuiToastBus::Error("Exception loading preset");
            });
    }


} // namespace soasim::ui
