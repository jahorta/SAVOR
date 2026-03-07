#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Core/Memory/Soa/SoaAddrCatalog.h"
#include "SearchableCombo.h"
#include "Core/Memory/Soa/SoaConstants.h"

namespace soasim::ui {

    enum class ProgKind : uint8_t {
        None = 0,
        TurnOrderIndex,
        ItemDropAmount,
        BattleTreasureSlotAmount,  // slot -> amount field
        EnemyItemAmount            // enemy slot, item index -> amount field
    };

    struct AddressProgramDraft {
        ProgKind kind{ ProgKind::None };
        uint16_t a{ 0 };
        uint16_t b{ 0 };
        std::string desc;
        std::vector<uint8_t> blob;

        bool Draw(const char* label, bool draw_separator = true);
        void Build();
    };

} // namespace soasim::ui
