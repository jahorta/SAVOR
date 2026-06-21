#include "SoldierActionExecutionModel.h"

#include <string>
#include <utility>

namespace savor::predict {

namespace {

std::string soldier_name(int slot) {
    return "[" + std::to_string(slot) + "]Soldier";
}

const PlannedAction* planned_action_for_slot(const PlannedTurnActions& actions, int slot) {
    if (slot == 4) {
        return &actions.soldier4;
    }
    if (slot == 5) {
        return &actions.soldier5;
    }
    return nullptr;
}

std::optional<int> find_turn_order_rank(const TurnOrderSimulation& turn_order, int slot) {
    if (!turn_order.execution_order_exact) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < turn_order.execution_slots.size(); ++i) {
        if (turn_order.execution_slots[i] == slot) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

std::optional<int> find_attack_event_order(const ParsedProgressEvents& events, int slot) {
    const auto actor = soldier_name(slot);
    for (std::size_t i = 0; i < events.ordered_combat_events.size(); ++i) {
        const auto& event = events.ordered_combat_events[i];
        if (event.kind == CombatEventKind::Attack && event.attack.actor == actor) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

std::optional<int> find_death_event_order(const ParsedProgressEvents& events, int slot) {
    const auto target = soldier_name(slot);
    for (std::size_t i = 0; i < events.ordered_combat_events.size(); ++i) {
        const auto& event = events.ordered_combat_events[i];
        if (event.kind == CombatEventKind::Death && event.target == target) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

SoldierActionExecution analyze_soldier(
    const ParsedProgressEvents& events,
    const TurnOrderSimulation& turn_order,
    int slot) {
    SoldierActionExecution result;
    result.slot = slot;
    result.turn_order_rank = find_turn_order_rank(turn_order, slot);
    result.attack_event_order = find_attack_event_order(events, slot);
    result.death_event_order = find_death_event_order(events, slot);

    if (events.planned_actions.has_value()) {
        if (const auto* planned = planned_action_for_slot(*events.planned_actions, slot)) {
            result.planned_kind = planned->kind;
        }
    }

    if (result.attack_event_order.has_value()) {
        result.status = SoldierActionExecutionStatus::ReachedExecution;
    } else if (!events.planned_actions.has_value()) {
        result.status = SoldierActionExecutionStatus::PlanMissing;
    } else if (result.planned_kind != PlannedActionKind::Attack) {
        result.status = SoldierActionExecutionStatus::NotPlannedAttack;
    } else if (result.death_event_order.has_value()) {
        result.status = SoldierActionExecutionStatus::PreventedByDeathBeforeAction;
    } else {
        result.status = SoldierActionExecutionStatus::PlannedAttackUnresolved;
    }

    return result;
}

} // namespace

SoldierActionExecutionSummary analyze_first_battle_soldier_action_execution(
    const ParsedProgressEvents& events,
    const TurnOrderSimulation& turn_order) {
    SoldierActionExecutionSummary summary;
    for (const int slot : {4, 5}) {
        auto soldier = analyze_soldier(events, turn_order, slot);
        if (soldier.planned_kind == PlannedActionKind::Attack) {
            ++summary.planned_attack_count;
        }
        switch (soldier.status) {
        case SoldierActionExecutionStatus::ReachedExecution:
            ++summary.reached_execution_count;
            break;
        case SoldierActionExecutionStatus::PreventedByDeathBeforeAction:
            ++summary.death_prevented_count;
            break;
        case SoldierActionExecutionStatus::PlannedAttackUnresolved:
            ++summary.unresolved_planned_attack_count;
            break;
        case SoldierActionExecutionStatus::PlanMissing:
        case SoldierActionExecutionStatus::NotPlannedAttack:
            break;
        }
        summary.soldiers.push_back(std::move(soldier));
    }
    return summary;
}

const char* soldier_action_execution_status_name(SoldierActionExecutionStatus status) {
    switch (status) {
    case SoldierActionExecutionStatus::PlanMissing:
        return "PlanMissing";
    case SoldierActionExecutionStatus::NotPlannedAttack:
        return "NotPlannedAttack";
    case SoldierActionExecutionStatus::ReachedExecution:
        return "ReachedExecution";
    case SoldierActionExecutionStatus::PreventedByDeathBeforeAction:
        return "PreventedByDeathBeforeAction";
    case SoldierActionExecutionStatus::PlannedAttackUnresolved:
        return "PlannedAttackUnresolved";
    default:
        return "Unknown";
    }
}

} // namespace savor::predict
