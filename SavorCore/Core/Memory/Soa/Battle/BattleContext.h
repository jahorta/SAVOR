#pragma once
#include <cstdint>
#include "../SoaStructs.h"  // contains dme::CombatantInstance and dme::EnemyDefinition
#include "../SoaConstants.h"

namespace soa::battle::ctx {

    enum StatusFlags : uint32_t {
        Dead = 1 << 8,
        Fled = 1 << 13,
    };

    static constexpr const int SLOT_COUNT = 12;

    struct BattleSlot
    {
        uint8_t  present{ 0 };       // slot has a live instance
        uint8_t  is_player{ 0 };     // 1 for slots [0..3]
        uint16_t id{ 0 };            // from 0x80309DCC
        uint8_t  is_alive{ 0 };
        uint8_t  has_enemy_def{ 0 }; // 1 iff enemy slot and enemy_def is resolved

        // Materialized snapshots (byte-faithful images from MEM1)
        soa::CombatantInstance instance{};     // valid only if present=1
        soa::EnemyDefinition   enemy_def{};    // valid only if has_enemy_def=1

        // Optional provenance (kept for debugging / inspection)
        uint32_t instance_addr{ 0 };
        uint32_t enemy_def_addr{ 0 };
    };

    struct BattleContext
    {
        BattleSlot slots_[SLOT_COUNT];
        soa::BattleState state{};
        soa::battle::TurnType turn_type;
        uint32_t turn_count;
        uint32_t battle_phase;

        BattleContext() = default;
    };

} // namespace savor::battlectx
