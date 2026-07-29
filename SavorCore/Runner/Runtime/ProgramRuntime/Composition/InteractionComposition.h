#pragma once

#include "SemanticObservationComposition.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program::composition {

enum class InteractionInputKind : std::uint8_t
{
    Held,
    Pulse,
    Neutral,
    Sequence,
};

enum class InputAcknowledgementPolicy : std::uint8_t
{
    NotRequired,
    RequestOnly,
    RequestAndRelease,
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
    ExactDependencyIdentity publish_held;
    ExactDependencyIdentity publish_pulse;
    ExactDependencyIdentity publish_sequence;
    ExactDependencyIdentity neutralize;
    ExactDependencyIdentity await_guest_poll;
    ExactDependencyIdentity subscribe_group;
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
    InputAcknowledgementPolicy acknowledgement =
        InputAcknowledgementPolicy::RequestAndRelease;
    // When behavior depends on the guest executing beyond the reached gate
    // under the same non-neutral publication, name the semantic successor
    // explicitly. Ordinary source departure uses ContinueUntil suppression.
    std::optional<SemanticPointReference> held_through_successor;
    std::uint64_t deadline_milliseconds = 0;
    bool fail_on_movie_end = true;
    bool require_vi_progress = true;
    std::vector<ProgramFunctionId> attached_observations;
    std::vector<ExactDependencyIdentity> attached_checks;
    std::optional<std::string> release_witness_point;
    std::optional<ExactDependencyIdentity> memory_change_observation;
    std::uint64_t memory_change_address = 0;
    TypeRef memory_change_value_type;
    std::uint32_t maximum_memory_polls = 0;
    ExactDependencyIdentity completion_mapper;
    std::optional<std::string> static_next_segment;

    auto operator<=>(const InteractionSegmentDefinition&) const = default;
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
    TypeRef subscription_type;
    TypeRef point_receipt_type;
    TypeRef input_publication_receipt_type;
    TypeRef input_neutral_witness_type;
    TypeRef input_poll_receipt_type;
    TypeRef segment_result_type;
    TypeRef adaptive_transition_type;
    TypeRef adaptive_segment_id_type;
    std::string adaptive_state_field = "state";
    std::string adaptive_segment_field = "segment";
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
