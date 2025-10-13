#pragma once
#include <optional>
#include <string>
#include <cstdint>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "DB/PredicateSpecRepo.h"

namespace soasim::ui {

    struct PredicateSpecPopup {
        bool open{ false };
        std::string modal_id = "PredicateSpecPopup";
        simcore::db::PredicateSpecRow row{};

        void OpenNew() { open = true; row = {}; ImGui::OpenPopup(modal_id.c_str()); }
        void OpenEdit(const simcore::db::PredicateSpecRow& existing) { open = true; row = existing; ImGui::OpenPopup(modal_id.c_str()); }

        // Returns {saved?, id}
        std::pair<bool, int64_t> Draw() {
            std::pair<bool, int64_t> res{ false, 0 };
            if (!open) return res;
            bool done = false;
            if (ImGui::BeginPopupModal(modal_id.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputInt("Spec Version", &row.spec_version);
                ImGui::InputInt("Required BP", &row.required_bp);
                ImGui::InputInt("Kind", &row.kind);
                ImGui::InputInt("Width", &row.width);
                ImGui::InputInt("Cmp Op", &row.cmp_op);
                ImGui::InputInt("Flags", &row.flags);
                ImGui::InputScalar("LHS Addr", ImGuiDataType_S64, &row.lhs_addr);
                int tmp{};
                if (row.lhs_key) { tmp = *row.lhs_key; }
                if (ImGui::InputInt("LHS Key (opt)", &tmp)) { row.lhs_key = tmp; }
                ImGui::InputScalar("RHS Value", ImGuiDataType_S64, &row.rhs_value);
                int tmp2{}; if (row.rhs_key) tmp2 = *row.rhs_key;
                if (ImGui::InputInt("RHS Key (opt)", &tmp2)) { row.rhs_key = tmp2; }
                ImGui::InputInt("Turn Mask", &row.turn_mask);
                int64_t pid{};
                if (row.lhs_prog_id) pid = *row.lhs_prog_id;
                if (ImGui::InputScalar("LHS Prog Id (opt)", ImGuiDataType_S64, &pid)) { row.lhs_prog_id = pid; }
                int64_t pid2{}; if (row.rhs_prog_id) pid2 = *row.rhs_prog_id;
                if (ImGui::InputScalar("RHS Prog Id (opt)", ImGuiDataType_S64, &pid2)) { row.rhs_prog_id = pid2; }
                ImGui::InputText("Description", &row.description);

                if (ImGui::Button("Save")) {
                    // TODO validate here
                    
                    // Prefer EnsureByFingerprint if you want dedupe; otherwise BulkInsert of 1 is ok.
                    // Here: just insert w/ ensure-by-fp if available; fallback to bulk insert.
                    simcore::db::DbResult<int64_t> r = simcore::db::PredicateSpecRepo::EnsureByFingerprint(row);
                    if (!r.ok) {
                        std::vector<simcore::db::PredicateSpecRow> v; v.push_back(row);
                        r = simcore::db::PredicateSpecRepo::BulkInsert(v);
                    }
                    if (r.ok) { res = { true, r.value }; done = true; }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { done = true; }
                ImGui::EndPopup();
            }
            if (done) { ImGui::CloseCurrentPopup(); open = false; }
            return res;
        }
    };

} // namespace soasim::ui
