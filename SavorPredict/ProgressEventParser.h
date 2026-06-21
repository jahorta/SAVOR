#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class PlannedActionKind {
    Unknown,
    Attack,
    Defend,
};

struct PlannedAction {
    PlannedActionKind kind = PlannedActionKind::Unknown;
    std::string target;
};

struct PlannedTurnActions {
    PlannedAction vyse;
    PlannedAction aika;
    PlannedAction soldier4;
    PlannedAction soldier5;
};

struct PcBeforeEcPredicate {
    int lhs = 0;
    int rhs = 0;
};

struct AttackEvent {
    std::string actor;
    std::string target;
    int damage = 0;
};

struct DropEvent {
    std::string target;
    std::string drop;
};

enum class CombatEventKind {
    Attack,
    Death,
    Drop,
};

struct CombatEvent {
    CombatEventKind kind = CombatEventKind::Attack;
    AttackEvent attack;
    std::string target;
    std::string drop;
};

struct ParsedProgressEvents {
    std::optional<PlannedTurnActions> planned_actions;
    std::optional<PcBeforeEcPredicate> pc_before_ec_predicate;
    std::vector<AttackEvent> attacks;
    std::vector<std::string> deaths;
    std::vector<DropEvent> drops;
    std::vector<CombatEvent> ordered_combat_events;
};

PlannedAction parse_planned_action(std::string_view text);
ParsedProgressEvents parse_progress_events(const std::vector<std::string>& messages);
std::string planned_action_to_string(const PlannedAction& action);

} // namespace savor::predict
