#include "AddrProgramEditor.h"
#include <type_traits>

namespace soasim::ui {

    using soasim::ui::widgets::SearchableComboState;

    static inline const char* ProgKindLabel(ProgKind k) {
        switch (k) {
        case ProgKind::None: return "None";
        case ProgKind::TurnOrderIndex: return "Turn order index (derived)";
        case ProgKind::ItemDropAmount: return "Item drop amount (derived)";
        case ProgKind::BattleTreasureSlotAmount: return "Battle treasure slot -> amount";
        case ProgKind::EnemyItemAmount: return "Enemy item -> amount";
        }
        return "Unknown";
    }

    bool AddressProgramDraft::Draw(const char* label, bool draw_separator)
    {
        bool changed = false;
        if (draw_separator) ImGui::SeparatorText(label);

        int mode = (int)kind;
        if (ImGui::BeginCombo("Program", ProgKindLabel(kind))) {
            for (int i = 0; i <= (int)ProgKind::EnemyItemAmount; ++i) {
                bool sel = (mode == i);
                if (ImGui::Selectable(ProgKindLabel((ProgKind)i), sel)) {
                    mode = i; changed = true;
                }
            }
            ImGui::EndCombo();
        }
        kind = (ProgKind)mode;

        switch (kind) {
        case ProgKind::None: break;
        case ProgKind::TurnOrderIndex:
            changed |= ImGui::InputScalar("logical id index (0..63)", ImGuiDataType_U16, &a);
            break;
        case ProgKind::ItemDropAmount: {
            changed |= ImGui::InputScalar("item id", ImGuiDataType_U16, &a);
            break;
        }
        case ProgKind::BattleTreasureSlotAmount:
            changed |= ImGui::InputScalar("treasure slot index", ImGuiDataType_U16, &a);
            break;
        case ProgKind::EnemyItemAmount:
            changed |= ImGui::InputScalar("enemy slot (0..7)", ImGuiDataType_U16, &a);
            changed |= ImGui::InputScalar("item idx (0..3)", ImGuiDataType_U16, &b);
            break;
        }

        if (ImGui::Button("Build program")) {
            Build();
            changed = true;
        }

        if (!blob.empty()) {
            ImGui::TextDisabled("Built: %zu bytes", blob.size());
            if (!desc.empty()) ImGui::SameLine(), ImGui::Text("— %s", desc.c_str());
        }

        return changed;
    }

    void AddressProgramDraft::Build()
    {
        blob.clear();
        desc.clear();
        if (kind == ProgKind::None) return;

        addrprog::Builder builder;
        std::string d;
        switch (kind) {
        case ProgKind::TurnOrderIndex:
            addrprog::catalog::turn_order_idx(builder, a, d);
            break;
        case ProgKind::ItemDropAmount:
            addrprog::catalog::item_drop_amt(builder, a, d);
            break;
        case ProgKind::BattleTreasureSlotAmount:
            addrprog::catalog::battle_treasure_slot(builder, a, &soa::BattleItemDropSlot::count, d);
            break;
        case ProgKind::EnemyItemAmount:
            addrprog::catalog::enemy_item_field(builder, a, b, &soa::ItemDrop::amount, d);
            break;
        default: break;
        }
        desc = d;
        blob = builder.blob();
    }

} // namespace soasim::ui
