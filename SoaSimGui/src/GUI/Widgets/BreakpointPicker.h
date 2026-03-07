#pragma once
#include <string>
#include <vector>
#include <optional>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

// Adjust include path to where your registry lives:
#include "././Runner/Breakpoints/BPRegistry.h"
#include "../Widgets/SegmentedControl.h"

namespace soasim::ui::widgets {

    struct BreakpointPickerState {
        int domain_idx{ 0 };             // 0: All, 1: PreBattle, 2: Battle, 3: Overworld
        std::string filter;            // substring match on name
        // cache is ephemeral; not persisted
    };

    inline const char* DomainLabel(int i) {
    switch (i) { case 1: return "PreBattle"; case 2: return "Battle"; case 3: return "Overworld"; default: return "All"; }
    }

    inline bool BreakpointPicker(const char* label, BPKey* inout_key, BreakpointPickerState& st)
    {
        bool changed = false;

        // Domain filter control
        {
            std::vector<std::string> tabs{ "All", "PreBattle", "Battle", "Overworld" };
            Segmented((std::string(label) + "##domain").c_str(), tabs, &st.domain_idx);
        }

        const char* preview = "(none)";
        if (inout_key && *inout_key != 0) {
            preview = bp::BPRegistry::name(*inout_key);
            if (!preview || !preview[0]) preview = "(unknown)";
        }

        if (ImGui::BeginCombo(label, preview)) {
            ImGui::InputText("search", &st.filter);

            auto all = bp::BPRegistry::all();
            for (const auto& r : all) {
                // domain filter
                if (st.domain_idx != 0) {
                    auto d = bp::domain_of(r.key);
                    if ((st.domain_idx == 1 && d != bp::BPDomain::PreBattle) ||
                        (st.domain_idx == 2 && d != bp::BPDomain::Battle) ||
                        (st.domain_idx == 3 && d != bp::BPDomain::Overworld)) {
                        continue;
                    }
                }
                // text filter
                if (!st.filter.empty()) {
                    std::string name = r.name ? r.name : "";
                    if (name.find(st.filter) == std::string::npos) continue;
                }
                bool sel = (inout_key && *inout_key == r.key);
                char buf[256];
                snprintf(buf, sizeof(buf), "%s  (0x%08X)", r.name ? r.name : "", r.pc);
                if (ImGui::Selectable(buf, sel)) {
                    if (inout_key) *inout_key = r.key;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        return changed;
    }

} // namespace soasim::ui::widgets
