#include "AuthoringContentHash.h"

#include <sstream>

namespace savor::db::authoring {
namespace {

std::string Fnv1a64(std::string_view prefix, std::string_view content)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto ch : content) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << prefix << std::hex << hash;
    return out.str();
}

} // namespace

std::string ComputeBattlePlanFingerprint(
    const std::string_view name,
    const std::span<const BattlePlanFingerprintTurn> turns)
{
    std::string content(name);
    content += '\n';
    for (const auto& turn : turns) {
        content += "turn:" + std::to_string(turn.turn_index) + "\n";
        content += "predicate_group:"
            + std::to_string(turn.predicate_group_revision_id.value_or(0)) + "\n";
        for (const auto& action : turn.actions) {
            content += "action:" + std::to_string(action.actor_slot)
                + ":" + std::to_string(action.action_preset_id)
                + ":" + std::to_string(action.ordinal) + "\n";
        }
    }
    return Fnv1a64("battle-plan-fnv1a64-", content);
}

std::string ComputeWorkflowGraphHash(
    const std::string_view name,
    const std::string_view description,
    const std::string_view execution_shape,
    const std::string_view expansion_kind,
    const std::span<const WorkflowGraphHashNode> nodes,
    const std::span<const WorkflowGraphHashEdge> edges)
{
    std::string content = "name:" + std::string(name)
        + "\ndescription:" + std::string(description)
        + "\nexecution_shape:" + std::string(execution_shape)
        + "\nexpansion_kind:" + std::string(expansion_kind) + "\n";
    for (const auto& node : nodes) {
        content += "node:" + std::string(node.node_key)
            + ":" + std::string(node.unit_kind) + "\n";
    }
    for (const auto& edge : edges) {
        content += "edge:" + std::string(edge.from_node_key)
            + "." + std::string(edge.output_key)
            + ">" + std::string(edge.to_node_key)
            + "." + std::string(edge.input_key)
            + ":" + std::string(edge.edge_kind) + "\n";
    }
    return Fnv1a64("fnv1a64-", content);
}

std::string ComputeStandaloneWorkflowGraphHash(
    const std::string_view unit_kind,
    const std::optional<std::string>& authored_ref_kind,
    const std::optional<std::int64_t>& authored_ref_id)
{
    std::string content = "standalone-unit:" + std::string(unit_kind) + "\n";
    if (authored_ref_kind.has_value() && authored_ref_id.has_value()) {
        content += "authored:" + *authored_ref_kind
            + ":" + std::to_string(*authored_ref_id) + "\n";
    }
    return Fnv1a64("standalone-fnv1a64-", content);
}

} // namespace savor::db::authoring
