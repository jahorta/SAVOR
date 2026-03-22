#pragma once
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <future>
#include <unordered_set>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "Core/Memory/Soa/SoaConstants.h"
#include "DB/TurnActionPresetRepo.h"
#include "DB/UiConfigRowRepo.h"
#include "DB/DBCore/DbResult.h"

#include "Utils/IniDoc.h"

#include "../../Components/ItemCatalog.h"
#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"

#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Phases/BattleExplorer.h"

namespace soasim::ui {

    struct UiActionPresetPopup {
        bool open{ false };
        bool want_focus{ false };
        std::string modal_id = "UiActionPresetPopup";

        simcore::db::TurnActionPresetRow row{};

        std::optional<int> ctx_turn_index{};
        std::optional<int> ctx_actor_slot{};

        const soa::battle::ctx::BattleContext* bc_{ nullptr };

        // Caches (rebuilt per-open or when bc changes explicitly)
        std::vector<int> present_enemy_kind_ids_;
        std::vector<uint16_t> present_item_ids_;
        std::unordered_set<uint16_t> present_item_set_;

        bool show_all_kinds_{ false };
        bool show_all_items_{ false };

        enum class TargetMode : uint8_t { Single, Multiple, Any, SameAsPC, ByKind };
        TargetMode target_mode{ TargetMode::Single };
        uint32_t   mask_bits{ 0 };
        int        single_slot{ 4 };
        int        same_pc_slot{ 0 };
        int        enemy_kind_id{ 0 };
        int        quantifier_idx{ 1 }; // 0=All,1=Any,2=First

        ItemType item_cat{ ItemType::All };
        soa::itemid::ItemId selected_item{ static_cast<soa::itemid::ItemId>(0) };

        bool saving_{ false };
        bool assigning_{ false };
        bool edit_id = 0;;
        int64_t saved_id = 0;

        void SetContext(const soa::battle::ctx::BattleContext* bc) {
            bc_ = bc;
            rebuild_caches_();
        }

        void Open() {
            open = want_focus = true; row = {};
            saving_ = assigning_ = false;
            edit_id = saved_id = 0;
            target_mode = TargetMode::Single; mask_bits = 0; single_slot = 4; same_pc_slot = 0; enemy_kind_id = 0; quantifier_idx = 1;
            item_cat = ItemType::All; selected_item = static_cast<soa::itemid::ItemId>(0);

            rebuild_caches_();
            ImGui::OpenPopup(modal_id.c_str());
        }

        void Open(const int64_t turn_action_id) {
            open = want_focus = true;
            saving_ = assigning_ = false;
            edit_id = saved_id = 0;
            target_mode = TargetMode::Single; mask_bits = 0; single_slot = 4; same_pc_slot = 0; enemy_kind_id = 0; quantifier_idx = 1;
            item_cat = ItemType::All; selected_item = static_cast<soa::itemid::ItemId>(0);

            auto existing = TurnActionPresetRepo::Get(turn_action_id);
            if (!existing.ok) GuiToastBus::Warn("Unable to retrieve turn action row");
            else {
                GuiToastBus::Success("Turn action row retrieved, loading...");

                edit_id = turn_action_id;

                row = existing.value;

                if (!row.target_expr_ini.empty()) {
                    IniDoc ini = IniDoc::parse(row.target_expr_ini);
                    const auto kind = ini.get("target", "kind", "");
                    if (kind == "ByEnemyKind") {
                        target_mode = TargetMode::ByKind;
                        enemy_kind_id = (int)ini.get_i64("target", "enemy_kind_id", 0);
                        const auto q = ini.get("target", "quantifier", "Any");
                        quantifier_idx = (q == "All") ? 0 : (q == "Any") ? 1 : 2;
                    }
                }
                else {
                    switch ((simcore::battleexplorer::TargetBindingKind)row.target_kind) {
                    default: case simcore::battleexplorer::TargetBindingKind::SingleEnemy:
                        target_mode = TargetMode::Single; single_slot = (row.single_slot <= 0) ? 4 : row.single_slot; break;
                    case simcore::battleexplorer::TargetBindingKind::MultipleEnemies:
                        target_mode = TargetMode::Multiple; mask_bits = row.mask_bits; break;
                    case simcore::battleexplorer::TargetBindingKind::AnyEnemy:
                        target_mode = TargetMode::Any; break;
                    case simcore::battleexplorer::TargetBindingKind::SameAsOtherPC:
                        target_mode = TargetMode::SameAsPC; same_pc_slot = row.same_as_pc; break;
                    }
                }
                selected_item = static_cast<soa::itemid::ItemId>(row.item_id);

            }

            rebuild_caches_();
            ImGui::OpenPopup(modal_id.c_str());
        }

        // Draw returns {saved?, id} when a save completes; otherwise {false,0}.
        std::pair<bool, int64_t> Draw() {
            if (!open) return {false, 0};

            // Make it a separate, non-dockable OS window; size/pos sane on first appear.
            ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoSavedSettings;

            // Center on first appear relative to main viewport (only as a starting point).
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(720, 0), ImVec2(vp->WorkSize.x, vp->WorkSize.y));

            if (want_focus) {
                ImGui::SetNextWindowFocus();   // brings this OS window/frontmost when it opens
            }

            if (ImGui::BeginPopupModal(modal_id.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {


                if (want_focus) { want_focus = false; }
                
                // Context banner
                if (bc_) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 220, 120, 255));
                    ImGui::TextUnformatted("BattleContext Loaded");
                    ImGui::PopStyleColor();
                }
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 150, 200, 255));
                    ImGui::TextUnformatted("No BattleContext");
                    ImGui::PopStyleColor();
                }
                ImGui::Separator();

                ImGui::InputText("Name", &row.name);

                // Macro picker
                using BA = soa::battle::actions::BattleAction;
                static const struct { BA v; const char* l; } MACROS[] = {
                    { BA::Attack, "Attack" }, { BA::Defend, "Defend" }, { BA::Focus, "Focus" }, { BA::UseItem, "Use Item" }
                };
                int macro_idx = 0;
                for (int i = 0; i < 4; ++i) if ((int)MACROS[i].v == row.macro) { macro_idx = i; break; }

                if (ImGui::BeginCombo("Action", MACROS[macro_idx].l)) {
                    for (int i = 0; i < 4; ++i) {
                        bool sel = (i == macro_idx);
                        bool dis = (MACROS[i].v == BA::UseItem) && bc_ && present_item_ids_.empty();
                        if (dis) ImGui::BeginDisabled(true);
                        if (ImGui::Selectable(MACROS[i].l, sel) && !dis) { macro_idx = i; row.macro = (int)MACROS[i].v; }
                        if (sel) ImGui::SetItemDefaultFocus();
                        if (dis) {
                            ImGui::EndDisabled();
                            ImGui::SetItemTooltip("No usable items in current context");
                        }
                    }
                    ImGui::EndCombo();
                }

                const bool needs_target = (row.macro == (int)BA::Attack || row.macro == (int)BA::UseItem);
                const bool needs_item = (row.macro == (int)BA::UseItem);
                const bool consumable_only = (row.macro == (int)BA::UseItem);

                if (needs_item) {
                    std::vector<soa::itemid::ItemId> ids;
                    ListByCategory(item_cat, ids);

                    if (ImGui::BeginCombo("Category", ItemTypeLabel(item_cat))) {
                        for (int t = 0; t <= (int)ItemType::Consumable; ++t) {
                            if (consumable_only && ItemType(t) != ItemType::Consumable) continue;
                            auto tt = (ItemType)t;
                            bool sel = (tt == item_cat);
                            if (ImGui::Selectable(ItemTypeLabel(tt), sel)) item_cat = tt;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    bool ctx_mode = bc_ != nullptr;
                    if (ctx_mode && !show_all_items_) {
                        std::vector<soa::itemid::ItemId> filtered;
                        filtered.reserve(present_item_ids_.size());
                        for (auto id : present_item_ids_) {
                            auto iid = static_cast<soa::itemid::ItemId>(id);
                            if (item_cat == ItemType::All || Classify(iid) == item_cat) filtered.push_back(iid);
                        }
                        ids.swap(filtered);
                    }

                    if (ctx_mode) {
                        ImGui::Checkbox("Show all items", &show_all_items_);
                        if (!show_all_items_ && present_item_ids_.empty()) {
                            ImGui::SameLine(); ImGui::TextDisabled("(none in inventory)");
                        }
                    }

                    std::string current_item_label;
                    {
                        const auto idx = (std::size_t)selected_item;
                        current_item_label = (idx < soa::text::ItemNames.size())
                            ? (std::to_string((int)selected_item) + " — " + std::string(soa::text::ItemNames[idx]))
                            : "(invalid)";
                    }
                    if (ImGui::BeginCombo("Item", current_item_label.c_str())) {
                        for (auto id : ids) {
                            const auto idx = (std::size_t)id;
                            const bool sel = (id == selected_item);
                            std::string label = std::to_string((int)id) + " — " + std::string(soa::text::ItemNames[idx]);
                            if (ImGui::Selectable(label.c_str(), sel)) selected_item = id;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    if (ctx_mode && !show_all_items_ && !present_item_set_.empty()) {
                        bool ok = present_item_set_.count((uint16_t)selected_item) > 0;
                        if (!ok) ImGui::TextDisabled("Selected item not in inventory");
                    }
                }

                if (needs_target) {
                    const char* TMODES[] = { "Single", "Multiple (mask)", "Any", "Same as PC", "By Enemy Kind" };
                    int m = (int)target_mode;
                    if (ImGui::BeginCombo("Target", TMODES[m])) {
                        for (int i = 0; i < 5; ++i) {
                            bool sel = (i == m);
                            if (ImGui::Selectable(TMODES[i], sel)) { m = i; }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    target_mode = (TargetMode)m;

                    if (target_mode == TargetMode::Single) {
                        for (int s = 4; s <= 11; ++s) {
                            bool present = !bc_ || bc_->slots_[s].present == 1;
                            ImGui::BeginDisabled(!present);
                            char buf[16]; snprintf(buf, sizeof(buf), "%d", s);
                            if (ImGui::RadioButton(buf, single_slot == s)) single_slot = s;
                            ImGui::SameLine();
                            ImGui::EndDisabled();
                        }
                        ImGui::NewLine();
                    }
                    else if (target_mode == TargetMode::Multiple) {
                        uint32_t new_mask = mask_bits;
                        for (int s = 4; s <= 11; ++s) {
                            bool present = !bc_ || bc_->slots_[s].present == 1;
                            ImGui::BeginDisabled(!present);
                            bool on = (new_mask & (1u << s)) != 0;
                            char buf[16]; snprintf(buf, sizeof(buf), "%d", s);
                            if (ImGui::Checkbox(buf, &on)) {
                                if (on) new_mask |= (1u << s); else new_mask &= ~(1u << s);
                            }
                            ImGui::SameLine();
                            ImGui::EndDisabled();
                        }
                        ImGui::NewLine();
                        mask_bits = new_mask;
                    }
                    else if (target_mode == TargetMode::SameAsPC) {
                        const char* labs[4] = { "Vyse","Aika","Fina","Drachma" };
                        int cur = same_pc_slot;
                        if (ImGui::BeginCombo("PC", labs[(cur >= 0 && cur < 4) ? cur : 0])) {
                            for (int s = 0; s <= 3; ++s) {
                                bool present = !bc_ || bc_->slots_[s].present == 1;
                                ImGui::BeginDisabled(!present);
                                bool sel = (s == cur);
                                if (ImGui::Selectable(labs[s], sel)) cur = s;
                                if (sel) ImGui::SetItemDefaultFocus();
                                ImGui::EndDisabled();
                            }
                            ImGui::EndCombo();
                        }
                        same_pc_slot = cur;
                    }
                    else if (target_mode == TargetMode::ByKind) {
                        std::vector<int> kinds = present_enemy_kind_ids_;
                        if (!bc_ || show_all_kinds_) {
                            kinds.clear();
                            kinds.reserve(soa::text::EnemyNames.size());
                            for (std::size_t i = 0; i < soa::text::EnemyNames.size(); ++i) kinds.push_back((int)i);
                        }

                        std::string cur_label = std::to_string(enemy_kind_id);
                        if ((std::size_t)enemy_kind_id < soa::text::EnemyNames.size()) {
                            cur_label += " — " + std::string(soa::text::EnemyNames[(std::size_t)enemy_kind_id]);
                        }
                        if (ImGui::BeginCombo("Enemy Kind", cur_label.c_str())) {
                            if (bc_) {
                                ImGui::Checkbox("Show all kinds", &show_all_kinds_);
                                if (!show_all_kinds_ && kinds.empty()) ImGui::TextDisabled("(no kinds present)");
                            }
                            for (int id : kinds) {
                                bool sel = (id == enemy_kind_id);
                                std::string label = std::to_string(id) + " — " + std::string(soa::text::EnemyNames[(std::size_t)id]);
                                if (ImGui::Selectable(label.c_str(), sel)) enemy_kind_id = id;
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        static const char* QLABS[] = { "All", "Any", "First" };
                        ImGui::Combo("Quantifier", &quantifier_idx, QLABS, 3);
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("Preview");
                ImGui::Text("macro=%d  item_id=%d  target_mode=%d", row.macro, (int)selected_item, (int)target_mode);
                ImGui::Separator();

                const bool ctx_mode = (bc_ != nullptr);
                const bool need_item_ok = (!needs_item) || (!ctx_mode) || show_all_items_ || present_item_set_.count((uint16_t)selected_item) > 0;
                bool need_target_ok = true;
                if (needs_target) {
                    if (target_mode == TargetMode::Single) {
                        need_target_ok = (!ctx_mode) || (bc_->slots_[single_slot].present == 1);
                    }
                    else if (target_mode == TargetMode::Multiple) {
                        uint32_t m = mask_bits;
                        if (ctx_mode) {
                            uint32_t any = 0;
                            for (int s = 4; s <= 11; ++s) if ((m & (1u << s)) && bc_->slots_[s].present == 1) { any = 1; break; }
                            need_target_ok = (any != 0);
                        }
                    }
                    else if (target_mode == TargetMode::SameAsPC) {
                        need_target_ok = (!ctx_mode) || (bc_->slots_[same_pc_slot].present == 1);
                    }
                    else if (target_mode == TargetMode::ByKind) {
                        if (!ctx_mode || show_all_kinds_) need_target_ok = true;
                        else {
                            bool any = false;
                            for (int s = 4; s <= 11; ++s) if (bc_->slots_[s].present == 1 && (int)bc_->slots_[s].id == enemy_kind_id) { any = true; break; }
                            need_target_ok = any;
                        }
                    }
                }

                bool need_name_ok = !row.name.empty();

                ImGui::BeginDisabled(saving_ || !need_item_ok || !need_target_ok || !need_name_ok);
                if (ImGui::Button("Save As New Preset")) {
                    write_back_to_row_();
                    saving_ = true;
                    FutureQueue::Enqueue(
                        simcore::db::TurnActionPresetRepo::InsertAsync(row),
                        [this](const DbResult<int64_t>& rr) {
                            saving_ = false;
                            if (rr.ok) 
                            { 
                                GuiToastBus::Success("Preset saved");
                                saved_id = rr.value;
                            }
                            else GuiToastBus::Error("Failed to save preset", rr.error.message);
                            open = false;
                        },
                        [this](std::exception_ptr) { saving_ = false; open = false; GuiToastBus::Error("Exception saving preset"); });
                }
                ImGui::SameLine();

                if (edit_id > 0) {
                    if (ImGui::Button("Update Current Preset")) {
                        write_back_to_row_();
                        saving_ = true;
                        FutureQueue::Enqueue(
                            simcore::db::TurnActionPresetRepo::UpdateAsync(row),
                            [this](const DbResult<void>& rr) {
                                saving_ = false;
                                if (rr.ok)
                                {
                                    GuiToastBus::Success("Preset saved");
                                    saved_id = edit_id;
                                }
                                else GuiToastBus::Error("Failed to save preset", rr.error.message);
                                open = false;
                            },
                            [this](std::exception_ptr) { saving_ = false; open = false; GuiToastBus::Error("Exception saving preset"); });
                    }
                }

                ImGui::EndDisabled();

                if (ImGui::Button("Cancel")) { open = false; }

                ImGui::EndPopup();
            }

            if (!open) { 
                ImGui::CloseCurrentPopup(); 
                return { true, saved_id };
            }
            return { false, 0 };
        }

    private:
        void rebuild_caches_() {
            present_enemy_kind_ids_.clear();
            present_item_ids_.clear();
            present_item_set_.clear();

            if (!bc_) return;

            bool seen[65536] = { false };
            for (int s = 4; s <= 11; ++s) {
                if (bc_->slots_[s].present == 1) {
                    int id = (int)bc_->slots_[s].id;
                    if (id >= 0 && id < 65536 && !seen[id]) { present_enemy_kind_ids_.push_back(id); seen[id] = true; }
                }
            }

            std::unordered_set<uint16_t> present_items;
            for (int i = 0; i < 80; i++)
            {
                if (bc_->state.useable_items[i].item_id == 0xFFFF) continue;
                present_item_set_.insert(bc_->state.useable_items[i].item_id);
                present_item_ids_.push_back(bc_->state.useable_items[i].item_id);
            }
        }

        void write_back_to_row_() {
            using BA = soa::battle::actions::BattleAction;
            if (row.macro == (int)BA::UseItem) row.item_id = (uint16_t)selected_item;

            row.target_expr_ini.clear();
            switch (target_mode) {
            case TargetMode::Single:
                row.target_kind = (int)simcore::battleexplorer::TargetBindingKind::SingleEnemy;
                row.single_slot = single_slot;
                row.mask_bits = (1u << single_slot);
                row.same_as_pc = 0xFF;
                break;
            case TargetMode::Multiple:
                row.target_kind = (int)simcore::battleexplorer::TargetBindingKind::MultipleEnemies;
                row.mask_bits = mask_bits;
                row.single_slot = 0xFF;
                row.same_as_pc = 0xFF;
                break;
            case TargetMode::Any:
                row.target_kind = (int)simcore::battleexplorer::TargetBindingKind::AnyEnemy;
                row.mask_bits = 0; row.single_slot = 0xFF; row.same_as_pc = 0xFF;
                break;
            case TargetMode::SameAsPC:
                row.target_kind = (int)simcore::battleexplorer::TargetBindingKind::SameAsOtherPC;
                row.same_as_pc = (uint8_t)same_pc_slot;
                row.mask_bits = 0; row.single_slot = 0xFF;
                break;
            case TargetMode::ByKind: {
                row.target_kind = (int)simcore::battleexplorer::TargetBindingKind::AnyEnemy; // neutral; expr drives actual domain
                IniDoc ini;
                ini.set("target", "kind", "ByEnemyKind");
                ini.set("target", "enemy_kind_id", std::to_string(enemy_kind_id));
                const char* q = (quantifier_idx == 0) ? "All" : (quantifier_idx == 1) ? "Any" : "First";
                ini.set("target", "quantifier", q);
                row.target_expr_ini = ini.to_string_preserve_order();
                row.mask_bits = 0; row.single_slot = 0xFF; row.same_as_pc = 0xFF;
                break;
            }
            }
        }
    };

} // namespace soasim::ui
