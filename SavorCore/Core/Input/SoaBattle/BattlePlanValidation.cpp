#include "BattlePlanValidation.h"

#include <array>
#include <format>

namespace soa::battle::actions {
namespace {

BattlePlanValidationResult Failure(
    BattlePlanValidationError error,
    std::uint32_t ordinal,
    std::string diagnostic)
{
    return {false, error, ordinal, std::move(diagnostic)};
}

bool HasItem(const soa::battle::ctx::BattleContext& context, std::uint16_t item_id)
{
    for (const auto& slot : context.state.useable_items)
        if (slot.item_id == item_id && slot.count > 0) return true;
    return false;
}

} // namespace

BattlePlanValidationResult ValidateBattleTurnPlan(
    const soa::battle::ctx::BattleContext& context,
    const BattleTurnExecutionSpec& plan)
{
    if (plan.commands.empty())
        return Failure(BattlePlanValidationError::EmptyPlan, 0,
            "Battle Plan has no commands for this turn");
    if (plan.fake_attack_count > 255)
        return Failure(BattlePlanValidationError::FakeAttackCountOutOfRange, 0,
            "fake-attack count exceeds the bounded command-entry contract");
    std::array<bool, 4> actors{};
    for (std::size_t index = 0; index < plan.commands.size(); ++index)
    {
        const auto& command = plan.commands[index];
        if (command.actor_slot >= 4 || !context.slots_[command.actor_slot].present
            || !context.slots_[command.actor_slot].is_player
            || !context.slots_[command.actor_slot].is_alive)
            return Failure(BattlePlanValidationError::InvalidActor,
                static_cast<std::uint32_t>(index),
                std::format("actor slot {} is not an available living party member", command.actor_slot));
        if (actors[command.actor_slot])
            return Failure(BattlePlanValidationError::DuplicateActor,
                static_cast<std::uint32_t>(index),
                std::format("actor slot {} has more than one command", command.actor_slot));
        actors[command.actor_slot] = true;

        switch (command.macro)
        {
        case BattleAction::Attack:
            if (command.params.target_slot == 0xffu) {
                bool alive = false;
                for (std::size_t slot = 4; slot < soa::battle::ctx::SLOT_COUNT; ++slot)
                    alive = alive || (context.slots_[slot].present
                        && !context.slots_[slot].is_player && context.slots_[slot].is_alive);
                if (!alive) return Failure(BattlePlanValidationError::InvalidTarget,
                    static_cast<std::uint32_t>(index), "no selectable enemy is available");
                break;
            }
            if (command.params.target_slot < 4 || command.params.target_slot >= 12
                || !context.slots_[command.params.target_slot].present
                || context.slots_[command.params.target_slot].is_player)
                return Failure(BattlePlanValidationError::InvalidTarget,
                    static_cast<std::uint32_t>(index),
                    std::format("target slot {} is not a selectable enemy", command.params.target_slot));
            if (!context.slots_[command.params.target_slot].is_alive)
                return Failure(BattlePlanValidationError::TargetNotAlive,
                    static_cast<std::uint32_t>(index),
                    std::format("target slot {} is not alive", command.params.target_slot));
            break;
        case BattleAction::Defend:
        case BattleAction::Focus:
            break;
        case BattleAction::UseItem:
            if (command.params.item_id == 0xffffu || !HasItem(context, command.params.item_id))
                return Failure(BattlePlanValidationError::ItemUnavailable,
                    static_cast<std::uint32_t>(index),
                    std::format("item {} is not available", command.params.item_id));
            return Failure(BattlePlanValidationError::UnsupportedCommand,
                static_cast<std::uint32_t>(index),
                "UseItem is represented and inventory-valid, but its interaction is not implemented");
        case BattleAction::FakeAttack:
        default:
            return Failure(BattlePlanValidationError::UnsupportedCommand,
                static_cast<std::uint32_t>(index),
                "Battle command form is not supported by battle.single_turn");
        }
    }
    return {true, BattlePlanValidationError::None, 0, {}};
}

} // namespace soa::battle::actions
