#pragma once

#include "SemanticObservationComposition.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program::composition {

enum class InteractionInputKind : std::uint8_t
{
    Held,
    Neutral,
};

struct InteractionParameter
{
    std::string name;
    TypeRef type;

    auto operator<=>(const InteractionParameter&) const = default;
};

struct InteractionActionSet
{
    ExactDependencyIdentity acquire_input_lease;
    ExactDependencyIdentity apply_input_state;
    ExactDependencyIdentity continue_until;
    ExactDependencyIdentity step_frames;

    auto operator<=>(const InteractionActionSet&) const = default;
};

struct InteractionSegmentDefinition
{
    std::string canonical_id;
    std::vector<SemanticPointReference> gate_alternatives;
    std::size_t requested_input_parameter = 0;
    InteractionInputKind input_kind = InteractionInputKind::Held;
    // When behavior depends on the guest executing beyond the reached gate
    // under the same non-neutral publication, name the semantic successor
    // explicitly. Ordinary source departure uses ContinueUntil suppression.
    std::optional<SemanticPointReference> held_through_successor;
    // Optional neutral-state gate reached after the held publication has been
    // acknowledged and released. This keeps multi-stage interactions within
    // one adaptive segment without carrying input or guest evidence forward.
    std::optional<SemanticPointReference> post_release_gate;
    bool fail_on_movie_end = true;
    std::vector<ProgramFunctionId> attached_observations;
    std::vector<ExactDependencyIdentity> attached_checks;
    std::optional<ExactDependencyIdentity> memory_change_observation;
    std::uint64_t memory_change_address = 0;
    TypeRef memory_change_value_type;
    std::uint32_t maximum_memory_polls = 0;
    // Additional neutral guest frames after the semantic gate and any
    // required memory-change synchronization have completed.
    std::uint32_t post_gate_neutral_frames = 0;
    ExactDependencyIdentity completion_mapper;
    std::optional<std::string> static_next_segment;

    auto operator<=>(const InteractionSegmentDefinition&) const = default;
};

struct InteractionAdaptiveSelectionMap
{
    // Reducer enum values map explicitly to canonical segment identities and
    // never depend on vector position or declaration order.
    std::map<std::int64_t, std::string> segments;
    std::int64_t complete = 0;

    auto operator<=>(const InteractionAdaptiveSelectionMap&) const = default;
};

struct InteractionDefinition
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string source_name;
    std::vector<InteractionParameter> parameters;
    TypeRef state_type;
    TypeRef output_type;
    TypeRef lease_type;
    TypeRef point_receipt_type;
    TypeRef input_execution_binding_type;
    TypeRef segment_result_type;
    TypeRef adaptive_transition_type;
    TypeRef adaptive_segment_id_type;
    std::string adaptive_state_field = "state";
    std::string adaptive_segment_field = "segment";
    std::optional<InteractionAdaptiveSelectionMap> adaptive_selection;
    ExactDependencyIdentity initialize_reducer;
    std::optional<ExactDependencyIdentity> advance_reducer;
    ExactDependencyIdentity finalize_reducer;
    InteractionActionSet actions;
    std::vector<InteractionSegmentDefinition> segments;
    std::string first_segment;
    ProgramBudgets budgets;
};

[[nodiscard]] CompositionResult LowerInteraction(
    const InteractionDefinition& definition,
    ProgramModule& module);

} // namespace savor::runtime::program::composition
