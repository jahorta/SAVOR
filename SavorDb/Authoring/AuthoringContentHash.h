#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace savor::db::authoring {

struct BattlePlanFingerprintAction {
    int actor_slot = 0;
    std::int64_t action_preset_id = 0;
    int ordinal = 0;
};

struct BattlePlanFingerprintTurn {
    int turn_index = 0;
    std::optional<std::int64_t> predicate_group_revision_id;
    std::span<const BattlePlanFingerprintAction> actions;
};

struct WorkflowGraphHashNode {
    std::string_view node_key;
    std::string_view unit_kind;
};

struct WorkflowGraphHashArgument {
    std::string_view node_key;
    std::string_view argument_key;
    std::string_view binding_mode;
    std::string_view constant_value;
};

struct WorkflowGraphHashEdge {
    std::string_view from_node_key;
    std::string_view output_key;
    std::string_view to_node_key;
    std::string_view input_key;
    std::string_view edge_kind = "DATA";
};

std::string ComputeBattlePlanFingerprint(
    std::string_view name,
    std::span<const BattlePlanFingerprintTurn> turns);

std::string ComputeWorkflowGraphHash(
    std::string_view name,
    std::string_view description,
    std::string_view execution_shape,
    std::string_view expansion_kind,
    std::span<const WorkflowGraphHashNode> nodes,
    std::span<const WorkflowGraphHashArgument> arguments,
    std::span<const WorkflowGraphHashEdge> edges);

std::string ComputeStandaloneWorkflowGraphHash(
    std::string_view unit_kind,
    const std::optional<std::string>& authored_ref_kind = std::nullopt,
    const std::optional<std::int64_t>& authored_ref_id = std::nullopt);

} // namespace savor::db::authoring
