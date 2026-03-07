#pragma once
#include <string>
#include <vector>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

namespace soasim::ui::widgets {

    struct SearchableComboState {
        std::string filter;
        int last_idx{ -1 };
    };

    inline bool SearchableCombo(const char* label,
        const std::vector<std::string>& items,
        int* current_index,
        SearchableComboState& st)
    {
        bool changed = false;
        const char* preview = (current_index && *current_index >= 0 && *current_index < (int)items.size())
            ? items[*current_index].c_str() : "(none)";
        if (ImGui::BeginCombo(label, preview)) {
            ImGui::InputText("search", &st.filter);
            for (int i = 0; i < (int)items.size(); ++i) {
                if (!st.filter.empty()) {
                    if (items[i].find(st.filter) == std::string::npos) continue;
                }
                const bool sel = (current_index && *current_index == i);
                if (ImGui::Selectable(items[i].c_str(), sel)) {
                    if (current_index) *current_index = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

} // namespace soasim::ui::widgets
