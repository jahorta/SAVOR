#pragma once
#include <string>
#include <vector>
#include "imgui.h"

namespace soasim::ui::widgets {

    inline bool Segmented(const char* id, const std::vector<std::string>& labels, int* idx, bool horizontal = true, bool two_per_row = false)
    {
        bool changed = false;
        ImGui::PushID(id);
        for (int i = 0; i < (int)labels.size(); ++i) {
            if (i && (horizontal || (two_per_row && i % 2 == 1))) ImGui::SameLine();
            ImGui::PushID(i);
            bool active = (idx && *idx == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(labels[i].c_str())) {
                if (idx && *idx != i) { *idx = i; changed = true; }
            }
            if (active) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }

} // namespace soasim::ui::widgets
