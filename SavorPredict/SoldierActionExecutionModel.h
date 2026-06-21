#pragma once

#include "ProgressEventParser.h"
#include "TurnOrderModel.h"

#include <optional>
#include <vector>

namespace savor::predict {

enum class SoldierActionExecutionStatus {
    PlanMissing,
    NotPlannedAttack,
    ReachedExecution,
    PreventedByDeathBeforeAction,
    PlannedAttackUnresolved,
};

struct SoldierActionExecution {
    int slot = 0;
    PlannedActionKind planned_kind = PlannedActionKind::Unknown;
    std::optional<int> turn_order_rank;
    std::optional<int> attack_event_order;
    std::optional<int> death_event_order;
    SoldierActionExecutionStatus status = SoldierActionExecutionStatus::PlanMissing;
};

struct SoldierActionExecutionSummary {
    std::vector<SoldierActionExecution> soldiers;
    int planned_attack_count = 0;
    int reached_execution_count = 0;
    int death_prevented_count = 0;
    int unresolved_planned_attack_count = 0;
};

SoldierActionExecutionSummary analyze_first_battle_soldier_action_execution(
    const ParsedProgressEvents& events,
    const TurnOrderSimulation& turn_order);
const char* soldier_action_execution_status_name(SoldierActionExecutionStatus status);

} // namespace savor::predict
