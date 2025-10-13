#pragma once
#include <optional>
#include <string>
#include <cstdint>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "DB/TurnActionPresetRepo.h"
#include "DB/Querying/DataService.h"

namespace soasim::ui {

    struct UiActionPresetPopup {
        bool open{ false };
        simcore::db::TurnActionPresetRow row{};
        std::string modal_id = "UiActionPresetPopup";

        void OpenNew() { open = true; row = {}; ImGui::OpenPopup(modal_id.c_str()); }
        void OpenEdit(const simcore::db::TurnActionPresetRow& existing) { open = true; row = existing; ImGui::OpenPopup(modal_id.c_str()); }

        // Returns {saved?, id}
        std::pair<bool, int64_t> Draw() {
            std::pair<bool, int64_t> res{ false, 0 };
            if (!open) return res;
            bool done = false;
            if (ImGui::BeginPopupModal(modal_id.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("Name", &row.name);
                ImGui::InputInt("Macro", &row.macro);
                ImGui::InputInt("TargetKind", &row.target_kind);
                ImGui::InputInt("SameAsPC", &row.same_as_pc);
                ImGui::InputInt("Flags", &row.flags);
                ImGui::InputTextMultiline("TargetExprIni", &row.target_expr_ini, ImVec2(420, 120));

                if (ImGui::Button("Save")) {
                    // TODO validate here

                    // Maybe we should do an Ensure here instead of just inserting everytime.
                    DbResult<int64_t> r = simcore::db::TurnActionPresetRepo::Insert(row);
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
