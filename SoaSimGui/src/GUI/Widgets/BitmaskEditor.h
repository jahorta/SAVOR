#pragma once
#include <cstdint>
#include <string>
#include "imgui.h"

namespace soasim::ui::widgets {

    inline bool TurnMaskEditor(const char* label, uint32_t* mask)
    {
        bool changed = false;
        if (!mask) return false;

        ImGui::SeparatorText(label);
        bool every = (*mask == 0 || *mask == 0xFFFFFFFFu);
        if (ImGui::Checkbox("Every turn", &every)) {
            *mask = every ? 0xFFFFFFFFu : 0u;
            changed = true;
        }
        ImGui::BeginDisabled(every);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 8; ++c) {
                int t = r * 8 + c + 1;
                ImGui::PushID(t);
                bool on = ((*mask) & (1u << (t - 1))) != 0;
                if (ImGui::Checkbox(std::to_string(t).c_str(), &on)) {
                    if (on) *mask |= (1u << (t - 1));
                    else *mask &= ~(1u << (t - 1));
                    changed = true;
                }
                ImGui::PopID();
                if (c < 7) ImGui::SameLine();
            }
        }
        ImGui::EndDisabled();
        return changed;
    }

} // namespace soasim::ui::widgets
