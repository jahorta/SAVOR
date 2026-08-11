#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <utility>
#include "../../Memory/Soa/SoaConstants.h"

namespace soa::battle::actions {

    enum class BattleAction : uint8_t {
        Attack = 0,
        Defend = 1,
        Focus = 2,
        FakeAttack = 3,
        UseItem = 4
    };

    struct BattleActionDefinition {
        BattleAction action;
        std::string_view name;
    };

    inline constexpr std::array<BattleActionDefinition, 5>
        BattleActionDefinitions{{
            {BattleAction::Attack, "Attack"},
            {BattleAction::Defend, "Defend"},
            {BattleAction::Focus, "Focus"},
            {BattleAction::FakeAttack, "FakeAttack"},
            {BattleAction::UseItem, "UseItem"},
        }};

    [[nodiscard]] constexpr const BattleActionDefinition*
    find_battle_action_definition(std::int64_t value) noexcept {
        for (const auto& definition : BattleActionDefinitions) {
            if (static_cast<std::int64_t>(definition.action) == value)
                return &definition;
        }
        return nullptr;
    }

    [[nodiscard]] consteval bool battle_action_definitions_are_complete() {
        if (BattleActionDefinitions.size() != 5) return false;
        for (std::size_t index = 0;
             index < BattleActionDefinitions.size(); ++index) {
            const auto& definition = BattleActionDefinitions[index];
            if (definition.name.empty()) return false;
            for (std::size_t other = 0; other < index; ++other) {
                if (BattleActionDefinitions[other].action == definition.action ||
                    BattleActionDefinitions[other].name == definition.name) {
                    return false;
                }
            }
        }
        return find_battle_action_definition(0) != nullptr &&
            find_battle_action_definition(1) != nullptr &&
            find_battle_action_definition(2) != nullptr &&
            find_battle_action_definition(3) != nullptr &&
            find_battle_action_definition(4) != nullptr;
    }

    static_assert(battle_action_definitions_are_complete());

    static constexpr size_t ACTION_PARAM_WIRE_SIZE = 3;
    struct ActionParameters {
        uint8_t target_slot = 0xFF; // single-target; 0xFF selects first available enemy
        uint16_t item_id = 0xFFFF; // valid when macro==UseItem

        /* Wire spec (uint8_t) (3 bytes)
        [0] target_slot
        [1..2] item_id
        */
        static void to_wire(const actions::ActionParameters ap, std::vector<std::uint8_t>& out) {
            out.push_back(ap.target_slot);
            out.push_back(static_cast<uint8_t>(ap.item_id & 0xFF));
            out.push_back(static_cast<uint8_t>((ap.item_id >> 8) & 0xFF));
        }
        static bool from_wire(const std::uint8_t*& cur, const std::uint8_t* end, actions::ActionParameters& ap) {
            if (end - cur < static_cast<std::ptrdiff_t>(ACTION_PARAM_WIRE_SIZE)) return false;

            ap.target_slot = *cur; cur += 1;
            ap.item_id = static_cast<uint16_t>(cur[0])
                | static_cast<uint16_t>(cur[1] << 8);
            cur += 2;
            return true;
        }
    };

    static constexpr size_t BATTLE_COMMAND_WIRE_SIZE = 2 + ACTION_PARAM_WIRE_SIZE;
    static constexpr size_t ACTION_PLAN_SIZE = BATTLE_COMMAND_WIRE_SIZE;
    struct BattleCommand {
        uint8_t actor_slot = 0;     // 0..3
        BattleAction macro{};
        ActionParameters params{};


        /* Wire spec (uint8_t) (5 bytes)
        [0] actor_slot
        [1] macro
        [2..4] ActionParameters (3 bytes)
        */
        static void to_wire(const actions::BattleCommand& ap, std::vector<std::uint8_t>& out) {
            out.push_back(ap.actor_slot);
            out.push_back(static_cast<uint8_t>(ap.macro));
            ActionParameters::to_wire(ap.params, out);
        }
        static bool from_wire(const std::uint8_t*& cur, const std::uint8_t* end, actions::BattleCommand& ap) {
            if (end - cur < static_cast<std::ptrdiff_t>(BATTLE_COMMAND_WIRE_SIZE)) return false;

            ap.actor_slot = *cur; cur += 1;
            const auto* action = find_battle_action_definition(*cur);
            if (action == nullptr) return false;
            ap.macro = action->action; cur += 1;
            ActionParameters::from_wire(cur, end, ap.params);
            return true;
        }
    };

    using BattleTurnCommandSet = std::vector<BattleCommand>;

    struct BattleTurnExecutionSpec {
        uint32_t     fake_attack_count = 0;
        BattleTurnCommandSet commands;
    };

    using BattleExecutionScript = std::vector<BattleTurnExecutionSpec>;

    // Compatibility names retained while call sites migrate to the resolved-command terminology.
    using ActionPlan = BattleCommand;
    using TurnPlanSpec = BattleTurnCommandSet;
    using TurnPlan = BattleTurnExecutionSpec;
    using BattlePath = BattleExecutionScript;

    inline std::string get_action_string(BattleAction a) {
        switch (a) {
        case BattleAction::Attack: return "Attack";
        case BattleAction::Defend: return "Guard";
        case BattleAction::Focus: return "Focus";
        case BattleAction::UseItem: return "UseItem";
        default: return "Unknown";
        }
    }

    inline int resolveTargetIndex(uint32_t slot) {
        // slot is already a concrete 0..11; anything else = "unset/auto"
        return (slot <= 11u) ? static_cast<int>(slot) : -1;
    }

    inline std::string get_battle_turn_execution_summary(BattleTurnExecutionSpec tp, std::string sep = "\n", bool offset = true) {
        std::string actor;
        for (auto sp : tp.commands) {
            actor = actor + sep + " [" + std::to_string(sp.actor_slot) + "] " + get_action_string(sp.macro);
            if (sp.macro == BattleAction::Attack)
                actor = actor + ":[" + std::to_string((sp.params.target_slot <= 11) ? sp.params.target_slot : 0xFF) + "]";
            if (sp.macro == BattleAction::UseItem)
            {
                actor = actor + ":[" + std::to_string(sp.params.item_id) + "]";
                actor = actor + ":[" + std::to_string((sp.params.target_slot <= 11) ? sp.params.target_slot : 0xFF) + "]";
            }
        }
        return actor;
    }

    inline std::string get_battle_execution_script_summary(BattleExecutionScript bp, std::string sep = "\n", bool offset = true) {
        std::vector<std::string> path;
        for (int i = 0; i < bp.size(); i++) {
            auto tp = bp[i];
            path.emplace_back(sep + (offset ? "    " : " ") + "Turn=" + std::to_string(i) + " FakeAtk:" + std::to_string(tp.fake_attack_count));
            path.emplace_back(get_battle_turn_execution_summary(tp, sep, offset));
        }

        std::string out;
        for (auto s : path) out.append(s);
        return out;
    }

    inline std::string get_turn_plan_summary(TurnPlan tp, std::string sep = "\n", bool offset = true) {
        return get_battle_turn_execution_summary(std::move(tp), std::move(sep), offset);
    }

    inline std::string get_battle_path_summary(BattlePath bp, std::string sep = "\n", bool offset = true) {
        return get_battle_execution_script_summary(std::move(bp), std::move(sep), offset);
    }

} // namespace soa::battle::actions
