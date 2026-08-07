#include "InteractionComposition.h"

#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace savor::runtime::program::composition {
namespace {

using detail::ModuleFragmentBuilder;

InstructionTarget ActionTarget(const ExactDependencyIdentity& dependency)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = dependency,
    };
}

InstructionTarget ReducerTarget(const ExactDependencyIdentity& dependency)
{
    return {
        .kind = InstructionTargetKind::Reducer,
        .dependency = dependency,
    };
}

class StaticConfigWriter final
{
public:
    explicit StaticConfigWriter(std::array<char, 4> magic)
    {
        for (const char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }
    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    }
    void String(std::string_view value)
    {
        if (value.size() >
            std::numeric_limits<std::uint32_t>::max())
        {
            throw std::length_error("static config text is too large");
        }
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& value)
    {
        bytes_.insert(
            bytes_.end(),
            value.bytes.begin(),
            value.bytes.end());
    }
    [[nodiscard]] std::vector<Byte> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<Byte> bytes_;
};

void AddCanonicalActionSchemaImports(
    ModuleFragmentBuilder& builder,
    CanonicalAction action)
{
    for (const auto& schema :
         CanonicalActionTypeSchemaClosure(action))
    {
        builder.AddTypeImport(schema);
    }
}

std::optional<ProgramValueId> AddStaticConfig(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::vector<Byte> bytes,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = CanonicalRuntimeType(schema);
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        type,
        {},
        {},
        std::move(selector),
        LiteralValue{
            .type = type,
            .payload = std::move(bytes),
        },
        scope);
}

std::optional<ProgramValueId> AddOptional(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::optional<ProgramValueId> value,
    std::string selector,
    ProgramScopeId scope)
{
    std::vector<ProgramValueId> operands;
    if (value)
        operands.push_back(*value);
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(schema),
        operands,
        {},
        std::move(selector),
        std::nullopt,
        scope);
}

std::optional<ProgramValueId> AddRequest(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    std::span<const ProgramValueId> fields,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(action),
        fields,
        {},
        std::move(selector),
        std::nullopt,
        scope);
}

std::vector<Byte> StopGroupConfig(
    std::span<const SemanticPointReference> points)
{
    StaticConfigWriter writer({'S', 'G', 'C', '1'});
    writer.U32(static_cast<std::uint32_t>(
        points.size()));
    for (const auto& point : points)
    {
        writer.String(point.capability_pack.canonical_id);
        writer.U32(point.capability_pack.version);
        writer.Hash(point.capability_pack.manifest_hash);
        writer.String(point.canonical_id);
        writer.U8(static_cast<std::uint8_t>(point.kind));
        writer.U32(point.physical_pc);
    }
    writer.U32(0);
    // Passive Observe/Pass, EndOnEpochChange, scoped. ContinueUntil clones
    // this definition into ExecutionEngine's sole foreground Wake group.
    writer.U8(0);
    writer.U8(0);
    writer.U8(1);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> ContinueConfig(
    const InteractionSegmentDefinition& segment)
{
    StaticConfigWriter writer({'C', 'U', 'C', '1'});
    writer.U8(1); // FutureOnly; suppress exact retained source re-entry.
    writer.Bool(true);
    writer.Bool(segment.fail_on_movie_end);
    writer.U8(0); // preserve throttle
    writer.U8(0); // reject unknown interruption
    return std::move(writer).Finish();
}

std::vector<Byte> AdvanceConfig(
    const InteractionSegmentDefinition& segment)
{
    StaticConfigWriter writer({'E', 'A', 'C', '1'});
    writer.U8(2u);
    writer.Bool(segment.fail_on_movie_end);
    writer.U8(0);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> LeaseConfig(
    bool require_neutral_acknowledgement)
{
    StaticConfigWriter writer({'I', 'L', 'C', '1'});
    writer.U32(0); // port
    writer.U32(0); // priority
    writer.Bool(true); // suspendable
    writer.Bool(true); // interruption-borrowable
    writer.Bool(require_neutral_acknowledgement);
    writer.Bool(false); // movie-exclusive
    return std::move(writer).Finish();
}

std::vector<Byte> PublicationConfig(
    InteractionInputKind kind,
    InputAcknowledgementPolicy acknowledgement)
{
    StaticConfigWriter writer({'I', 'P', 'C', '1'});
    writer.U8(static_cast<std::uint8_t>(kind));
    writer.U8(static_cast<std::uint8_t>(acknowledgement));
    return std::move(writer).Finish();
}

std::vector<Byte> NeutralConfig(
    bool require_acknowledgement)
{
    StaticConfigWriter writer({'I', 'N', 'C', '1'});
    writer.Bool(require_acknowledgement);
    writer.Bool(true); // cleanup-safe neutral publication
    return std::move(writer).Finish();
}

std::vector<Byte> PollConfig(
    bool release,
    std::string_view witness,
    std::uint32_t retry_limit)
{
    StaticConfigWriter writer({'I', 'G', 'P', '1'});
    writer.Bool(release);
    writer.String(witness);
    writer.U32(retry_limit);
    writer.Bool(true);
    return std::move(writer).Finish();
}

std::vector<Byte> ObservationConfig(
    std::string_view observation_id)
{
    StaticConfigWriter writer({'O', 'S', 'C', '1'});
    writer.String(observation_id);
    writer.U8(0); // required
    writer.Bool(false);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::optional<ProgramValueId> AddMemoryReadRequest(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    ProgramValueId stop,
    std::uint64_t address,
    std::string selector,
    ProgramScopeId scope)
{
    const auto stop_value = AddOptional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::
            OptionalContinueUntilResult,
        stop,
        selector + "/stop-receipt",
        scope);
    const auto address_value = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        TypeRef::Builtin(BuiltinType::U64),
        {},
        {},
        selector + "/address",
        LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::U64),
            .payload = address,
        },
        scope);
    const auto config = AddStaticConfig(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::
            ObservationStaticConfig,
        ObservationConfig(selector),
        selector + "/static-config",
        scope);
    if (!stop_value || !address_value || !config)
        return std::nullopt;
    return AddRequest(
        builder,
        function,
        block,
        action,
        std::array{
            *stop_value,
            *address_value,
            *config},
        selector + "/request",
        scope);
}

std::string GateLabel(const InteractionSegmentDefinition& segment)
{
    std::string text = "gate[";
    for (std::size_t index = 0; index < segment.gate_alternatives.size(); ++index)
    {
        if (index != 0)
            text += ',';
        text += segment.gate_alternatives[index].canonical_id;
    }
    text += ']';
    return text;
}

const InteractionSegmentDefinition* FindSegment(
    const InteractionDefinition& definition,
    const std::string& id)
{
    const auto found = std::ranges::find(
        definition.segments,
        id,
        &InteractionSegmentDefinition::canonical_id);
    return found == definition.segments.end() ? nullptr : &*found;
}

std::optional<CompositionResult> Validate(
    const InteractionDefinition& definition)
{
    if (definition.canonical_id.empty() || definition.revision == 0)
        return detail::Fail(
            "interaction.invalid_identity",
            "interaction identity and revision are required");
    if (definition.segments.empty() || definition.first_segment.empty() ||
        FindSegment(definition, definition.first_segment) == nullptr)
    {
        return detail::Fail(
            "interaction.invalid_entry",
            "interaction requires a finite segment set and valid first segment");
    }
    if (definition.initialize_reducer.canonical_id.empty() ||
        definition.finalize_reducer.canonical_id.empty())
    {
        return detail::Fail(
            "interaction.missing_reducer",
            "interaction requires exact initialization and finalization reducers");
    }
    if (definition.budgets.maximum_instructions == 0 ||
        definition.budgets.maximum_action_requests == 0)
    {
        return detail::Fail(
            "interaction.unbounded",
            "interaction requires finite instruction and action budgets");
    }

    const auto& actions = definition.actions;
    const std::array required_actions{
        actions.acquire_input_lease.canonical_id,
        actions.publish_held.canonical_id,
        actions.publish_pulse.canonical_id,
        actions.publish_sequence.canonical_id,
        actions.neutralize.canonical_id,
        actions.await_guest_poll.canonical_id,
        actions.subscribe_group.canonical_id,
        actions.continue_until.canonical_id,
        actions.step_frames.canonical_id,
    };
    if (std::ranges::any_of(
            required_actions,
            [](const std::string& id) { return id.empty(); }))
    {
        return detail::Fail(
            "interaction.missing_action",
            "interaction action set must contain exact dependencies");
    }
    const InteractionActionSet canonical_actions{
        .acquire_input_lease = CanonicalActionIdentity(
            CanonicalAction::InputAcquireLease),
        .publish_held = CanonicalActionIdentity(
            CanonicalAction::InputPublishHeld),
        .publish_pulse = CanonicalActionIdentity(
            CanonicalAction::InputPublishPulse),
        .publish_sequence = CanonicalActionIdentity(
            CanonicalAction::InputPublishSequence),
        .neutralize = CanonicalActionIdentity(
            CanonicalAction::InputNeutralize),
        .await_guest_poll = CanonicalActionIdentity(
            CanonicalAction::InputAwaitGuestPoll),
        .subscribe_group = CanonicalActionIdentity(
            CanonicalAction::StopPointsSubscribeGroup),
        .continue_until = CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntil),
        .step_frames = CanonicalActionIdentity(
            CanonicalAction::ExecutionStepFrames),
    };
    if (actions != canonical_actions ||
        definition.lease_type != CanonicalActionOutputType(
            CanonicalAction::InputAcquireLease) ||
        definition.subscription_type != CanonicalActionOutputType(
            CanonicalAction::StopPointsSubscribeGroup) ||
        definition.point_receipt_type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil) ||
        definition.input_publication_receipt_type !=
            CanonicalActionOutputType(
                CanonicalAction::InputPublishHeld) ||
        definition.input_neutral_witness_type !=
            CanonicalActionOutputType(
                CanonicalAction::InputNeutralize) ||
        definition.input_poll_receipt_type !=
            CanonicalActionOutputType(
                CanonicalAction::InputAwaitGuestPoll))
    {
        return detail::Fail(
            "interaction.noncanonical_runtime_contract",
            "interaction actions and runtime values must use the exact canonical contracts");
    }

    std::set<std::string> parameter_names;
    for (const auto& parameter : definition.parameters)
    {
        if (parameter.name.empty() ||
            !parameter_names.insert(parameter.name).second)
        {
            return detail::Fail(
                "interaction.invalid_parameter",
                "interaction parameters require unique identities");
        }
    }

    std::set<std::string> segment_ids;
    for (const auto& segment : definition.segments)
    {
        if (segment.canonical_id.empty() ||
            !segment_ids.insert(segment.canonical_id).second ||
            segment.gate_alternatives.empty() ||
            segment.requested_input_parameter >= definition.parameters.size() ||
            segment.completion_mapper.canonical_id.empty())
        {
            return detail::Fail(
                "interaction.invalid_segment",
                "segments require unique identity, a gate, input, and completion mapping");
        }
        for (const auto& point : segment.gate_alternatives)
        {
            if (point.canonical_id.empty() ||
                (point.kind == SemanticPointKind::ProgramCounter &&
                 point.physical_pc == 0))
            {
                return detail::Fail(
                    "interaction.invalid_gate",
                "segment gates require exact nonzero semantic points");
            }
        }
        const TypeRef requested_type =
            definition.parameters[
                segment.requested_input_parameter].type;
        const TypeRef expected_input =
            segment.input_kind ==
                    InteractionInputKind::Sequence
            ? CanonicalRuntimeType(
                  CanonicalRuntimeSchema::
                      InputSequencePayload)
            : CanonicalRuntimeType(
                  CanonicalRuntimeSchema::
                      InputFramePayload);
        if (segment.input_kind !=
                InteractionInputKind::Neutral &&
            requested_type != expected_input)
        {
            return detail::Fail(
                "interaction.input_type_mismatch",
                "held and pulse inputs require InputFramePayload; sequences require InputSequencePayload");
        }
        if (segment.acknowledgement ==
                InputAcknowledgementPolicy::RequestAndRelease &&
            !segment.release_witness_point)
        {
            return detail::Fail(
                "interaction.missing_release_witness",
                "release acknowledgement requires a separately named witness");
        }
        if (segment.input_kind == InteractionInputKind::Neutral &&
            (segment.acknowledgement !=
                 InputAcknowledgementPolicy::NotRequired ||
             segment.held_through_successor))
        {
            return detail::Fail(
                "interaction.invalid_neutral_segment",
                "neutral segments cannot request held-input acknowledgement or a held-through semantic successor");
        }
        if (segment.held_through_successor &&
            (segment.held_through_successor->canonical_id.empty() ||
             (segment.held_through_successor->kind ==
                  SemanticPointKind::ProgramCounter &&
              segment.held_through_successor->physical_pc == 0)))
        {
            return detail::Fail(
                "interaction.invalid_held_successor",
                "held-through behavior requires an exact nonzero semantic successor");
        }
        if (segment.memory_change_observation &&
            (segment.maximum_memory_polls == 0 ||
             segment.memory_change_address == 0))
        {
            return detail::Fail(
                "interaction.unbounded_memory_poll",
                "memory-change observation requires a finite poll count");
        }
        if (segment.memory_change_observation)
        {
            const auto action =
                FindCanonicalAction(
                    *segment.memory_change_observation);
            if (!action ||
                (*action != CanonicalAction::GuestReadU8 &&
                 *action != CanonicalAction::GuestReadU16 &&
                 *action != CanonicalAction::GuestReadU32 &&
                 *action != CanonicalAction::GuestReadU64) ||
                CanonicalActionOutputType(*action) !=
                    segment.memory_change_value_type)
            {
                return detail::Fail(
                    "interaction.invalid_memory_observation",
                    "memory-change polling requires an exact canonical scalar-read action and result type");
            }
        }
    }
    for (const auto& segment : definition.segments)
    {
        if (segment.static_next_segment &&
            !segment_ids.contains(*segment.static_next_segment))
        {
            return detail::Fail(
                "interaction.unknown_next_segment",
                "static next segment must belong to the declared finite set");
        }
    }
    if (definition.advance_reducer)
    {
        if (definition.adaptive_state_field.empty() ||
            definition.adaptive_segment_field.empty() ||
            definition.adaptive_state_field ==
                definition.adaptive_segment_field)
        {
            return detail::Fail(
                "interaction.invalid_adaptive_projection",
                "adaptive transitions require distinct state and segment record fields");
        }
        for (const auto& segment : definition.segments)
        {
            if (segment.static_next_segment)
            {
                return detail::Fail(
                    "interaction.mixed_transition_modes",
                    "adaptive interactions cannot also declare static transitions");
            }
        }
    }
    return std::nullopt;
}

std::optional<ProgramValueId> ConstantU64(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint64_t value,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        TypeRef::Builtin(BuiltinType::U64),
        {},
        {},
        std::move(selector),
        LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::U64),
            .payload = value,
        },
        scope);
}

const ExactDependencyIdentity& PublicationAction(
    const InteractionActionSet& actions,
    InteractionInputKind kind)
{
    switch (kind)
    {
    case InteractionInputKind::Held:
        return actions.publish_held;
    case InteractionInputKind::Pulse:
        return actions.publish_pulse;
    case InteractionInputKind::Sequence:
        return actions.publish_sequence;
    case InteractionInputKind::Neutral:
        return actions.neutralize;
    }
    return actions.publish_held;
}

void AddActionImports(
    ModuleFragmentBuilder& builder,
    const InteractionActionSet& actions)
{
    builder.AddActionImport(actions.acquire_input_lease);
    builder.AddActionImport(actions.publish_held);
    builder.AddActionImport(actions.publish_pulse);
    builder.AddActionImport(actions.publish_sequence);
    builder.AddActionImport(actions.neutralize);
    builder.AddActionImport(actions.await_guest_poll);
    builder.AddActionImport(actions.subscribe_group);
    builder.AddActionImport(actions.continue_until);
    builder.AddActionImport(actions.step_frames);
    for (const auto action : {
             CanonicalAction::InputAcquireLease,
             CanonicalAction::InputPublishHeld,
             CanonicalAction::InputPublishPulse,
             CanonicalAction::InputPublishSequence,
             CanonicalAction::InputNeutralize,
             CanonicalAction::InputAwaitGuestPoll,
             CanonicalAction::StopPointsSubscribeGroup,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::ExecutionStepFrames,
         })
    {
        AddCanonicalActionSchemaImports(builder, action);
    }
}

} // namespace

CompositionResult LowerInteraction(
    const InteractionDefinition& definition,
    ProgramModule& module)
{
    if (const auto invalid = Validate(definition))
        return *invalid;

    ProgramModule candidate = module;
    ModuleFragmentBuilder builder(
        candidate,
        definition.source_name.empty()
            ? definition.canonical_id
            : definition.source_name,
        std::format(
            "interaction/{}/{}",
            definition.canonical_id,
            definition.revision));
    AddActionImports(builder, definition.actions);
    for (const auto& parameter : definition.parameters)
        builder.AddTypeImport(parameter.type);
    for (const auto& type : {
             definition.state_type,
             definition.output_type,
             definition.lease_type,
             definition.subscription_type,
             definition.point_receipt_type,
             definition.input_publication_receipt_type,
             definition.input_neutral_witness_type,
             definition.input_poll_receipt_type,
             definition.segment_result_type,
             definition.adaptive_transition_type,
             definition.adaptive_segment_id_type,
         })
    {
        builder.AddTypeImport(type);
    }
    builder.AddReducerImport(definition.initialize_reducer);
    builder.AddReducerImport(definition.finalize_reducer);
    if (definition.advance_reducer)
        builder.AddReducerImport(*definition.advance_reducer);
    for (const auto& segment : definition.segments)
    {
        for (const auto& point : segment.gate_alternatives)
            builder.AddCapabilityImport(point.capability_pack);
        builder.AddReducerImport(segment.completion_mapper);
        for (const auto& check : segment.attached_checks)
            builder.AddReducerImport(check);
        if (segment.memory_change_observation)
            builder.AddActionImport(*segment.memory_change_observation);
    }

    std::vector<ValueDefinition> arguments;
    arguments.reserve(definition.parameters.size());
    for (const auto& parameter : definition.parameters)
        arguments.push_back(builder.NewArgument(parameter.type));

    auto& function = builder.AddFunction(
        "interact." + definition.canonical_id,
        arguments,
        definition.output_type);
    function.blocks.reserve(definition.segments.size() + 2);
    auto& entry = builder.AddBlock(function);
    const auto outer_scope = builder.NewScope();
    (void)builder.AddInstruction(
        function,
        entry,
        InstructionOpcode::EnterScope,
        std::nullopt,
        {},
        {},
        "scope/input-lease",
        std::nullopt,
        outer_scope);

    const bool requires_neutral_acknowledgement =
        std::ranges::any_of(
            definition.segments,
            [](const InteractionSegmentDefinition& segment)
            {
                return segment.acknowledgement ==
                    InputAcknowledgementPolicy::
                        RequestAndRelease;
            });
    const auto lease_config = AddStaticConfig(
        builder,
        function,
        entry,
        CanonicalRuntimeSchema::InputLeaseStaticConfig,
        LeaseConfig(requires_neutral_acknowledgement),
        "input/acquire-lease/static-config",
        outer_scope);
    const auto lease_request = lease_config
        ? AddRequest(
              builder,
              function,
              entry,
              CanonicalAction::InputAcquireLease,
              std::array{*lease_config},
              "input/acquire-lease/request",
              outer_scope)
        : std::nullopt;
    if (!lease_request)
        return detail::Fail(
            "interaction.lowering_failed",
            "input lease request could not be constructed");
    const auto lease = builder.AddInstruction(
        function,
        entry,
        InstructionOpcode::AwaitAction,
        definition.lease_type,
        std::array{*lease_request},
        ActionTarget(definition.actions.acquire_input_lease),
        "input/acquire-lease",
        std::nullopt,
        outer_scope);
    if (!lease)
        return detail::Fail(
            "interaction.lowering_failed",
            "input lease action did not produce a resource");

    const auto unwind_neutral_config = AddStaticConfig(
        builder,
        function,
        entry,
        CanonicalRuntimeSchema::InputNeutralStaticConfig,
        NeutralConfig(requires_neutral_acknowledgement),
        "unwind/neutralize-input/static-config",
        outer_scope);
    const auto unwind_neutral_request =
        unwind_neutral_config
        ? AddRequest(
              builder,
              function,
              entry,
              CanonicalAction::InputNeutralize,
              std::array{
                  *lease,
                  *unwind_neutral_config},
              "unwind/neutralize-input/request",
              outer_scope)
        : std::nullopt;
    if (!unwind_neutral_request)
        return detail::Fail(
            "interaction.lowering_failed",
            "neutral compensation request could not be constructed");
    (void)builder.AddInstruction(
        function,
        entry,
        InstructionOpcode::DeferCompensation,
        std::nullopt,
        std::array{*unwind_neutral_request},
        {
            .kind = InstructionTargetKind::DeferredAction,
            .dependency = definition.actions.neutralize,
        },
        "unwind/neutralize-input",
        std::nullopt,
        outer_scope);

    std::vector<ProgramValueId> argument_ids;
    argument_ids.reserve(arguments.size());
    for (const auto& argument : arguments)
        argument_ids.push_back(argument.id);
    const auto state = builder.AddInstruction(
        function,
        entry,
        InstructionOpcode::CallReducer,
        definition.state_type,
        argument_ids,
        ReducerTarget(definition.initialize_reducer),
        "reducer/initialize",
        std::nullopt,
        outer_scope);
    if (!state)
        return detail::Fail(
            "interaction.lowering_failed",
            "initializer did not produce typed state");

    std::map<std::string, ProgramBlockId> segment_blocks;
    for (const auto& segment : definition.segments)
    {
        const auto state_argument = builder.NewArgument(definition.state_type);
        auto& block = builder.AddBlock(function, std::array{state_argument});
        segment_blocks.emplace(segment.canonical_id, block.id);
    }
    const auto final_state_argument = builder.NewArgument(definition.state_type);
    const auto final_result_argument =
        builder.NewArgument(definition.segment_result_type);
    auto& complete = builder.AddBlock(
        function,
        std::array{final_state_argument, final_result_argument});

    builder.SetTerminator(
        function,
        entry,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = segment_blocks.at(definition.first_segment),
                .arguments = {*state},
            }},
        },
        "dispatch/first-segment");

    for (std::size_t segment_index = 0;
         segment_index < definition.segments.size();
         ++segment_index)
    {
        const auto& segment = definition.segments[segment_index];
        auto& block = *std::ranges::find(
            function.blocks,
            segment_blocks.at(segment.canonical_id),
            &BasicBlock::id);
        const auto current_state = block.arguments.front().id;
        const auto segment_scope = builder.NewScope();
        (void)builder.AddInstruction(
            function,
            block,
            InstructionOpcode::EnterScope,
            std::nullopt,
            {},
            {},
            "segment/" + segment.canonical_id + "/scope",
            std::nullopt,
            segment_scope);

        const auto gate_config = AddStaticConfig(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::StopGroupStaticConfig,
            StopGroupConfig(segment.gate_alternatives),
            "segment/" + segment.canonical_id +
                "/gate/static-config",
            segment_scope);
        const auto gate_request = gate_config
            ? AddRequest(
                  builder,
                  function,
                  block,
                  CanonicalAction::
                      StopPointsSubscribeGroup,
                  std::array{*gate_config},
                  "segment/" + segment.canonical_id +
                      "/gate/request",
                  segment_scope)
            : std::nullopt;
        if (!gate_request)
            return detail::Fail(
                "interaction.lowering_failed",
                "segment gate request could not be constructed");
        const auto subscription = builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            definition.subscription_type,
            std::array{*gate_request},
            ActionTarget(definition.actions.subscribe_group),
            "segment/" + segment.canonical_id + "/" + GateLabel(segment),
            std::nullopt,
            segment_scope);
        const auto& publish_action = PublicationAction(
            definition.actions,
            segment.input_kind);
        const auto requested_input =
            arguments[segment.requested_input_parameter].id;
        const CanonicalAction publish_kind =
            segment.input_kind ==
                    InteractionInputKind::Held
            ? CanonicalAction::InputPublishHeld
            : segment.input_kind ==
                    InteractionInputKind::Pulse
            ? CanonicalAction::InputPublishPulse
            : segment.input_kind ==
                    InteractionInputKind::Sequence
            ? CanonicalAction::InputPublishSequence
            : CanonicalAction::InputNeutralize;
        const auto publish_config = AddStaticConfig(
            builder,
            function,
            block,
            publish_kind ==
                    CanonicalAction::InputNeutralize
                ? CanonicalRuntimeSchema::
                      InputNeutralStaticConfig
                : CanonicalRuntimeSchema::
                      InputPublicationStaticConfig,
            publish_kind ==
                    CanonicalAction::InputNeutralize
                ? NeutralConfig(
                      segment.acknowledgement ==
                      InputAcknowledgementPolicy::
                          RequestAndRelease)
                : PublicationConfig(
                      segment.input_kind,
                      segment.acknowledgement),
            "segment/" + segment.canonical_id +
                "/publish/static-config",
            segment_scope);
        std::vector<ProgramValueId> publish_fields{*lease};
        if (publish_kind != CanonicalAction::InputNeutralize)
            publish_fields.push_back(requested_input);
        publish_fields.push_back(*publish_config);
        const auto publish_request = AddRequest(
            builder,
            function,
            block,
            publish_kind,
            publish_fields,
            "segment/" + segment.canonical_id +
                "/publish/request",
            segment_scope);
        const auto publication = builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            publish_kind ==
                    CanonicalAction::InputNeutralize
                ? definition.input_neutral_witness_type
                : definition.input_publication_receipt_type,
            std::array{*publish_request},
            ActionTarget(publish_action),
            "segment/" + segment.canonical_id +
                "/publish-before-departure",
            std::nullopt,
            {});

        const auto wait_publication = AddOptional(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::
                OptionalInputPublicationReceipt,
            publish_kind ==
                    CanonicalAction::InputNeutralize
                ? std::nullopt
                : publication,
            "segment/" + segment.canonical_id +
                "/continue/input-publication",
            segment_scope);
        const auto wait_config = AddStaticConfig(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::
                ContinueUntilStaticConfig,
            ContinueConfig(segment),
            "segment/" + segment.canonical_id +
                "/continue/static-config",
            segment_scope);
        const auto no_movie = AddOptional(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::OptionalMoviePlaybackSession,
            std::nullopt,
            "segment/" + segment.canonical_id +
                "/continue/no-movie",
            segment_scope);
        const auto no_expected_count = AddOptional(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::OptionalMovieInputCount,
            std::nullopt,
            "segment/" + segment.canonical_id +
                "/continue/no-expected-count",
            segment_scope);
        const auto wait_request = AddRequest(
            builder,
            function,
            block,
            CanonicalAction::ExecutionContinueUntil,
            std::array{
                *subscription,
                *wait_publication,
                *no_movie,
                *no_expected_count,
                *wait_config},
            "segment/" + segment.canonical_id +
                "/continue/request",
            segment_scope);
        auto stop = builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            definition.point_receipt_type,
            std::array{*wait_request},
            ActionTarget(definition.actions.continue_until),
            "segment/" + segment.canonical_id +
                "/exact-stop-and-input-epoch/" + GateLabel(segment),
            std::nullopt,
            {});
        if (!stop)
        {
            return detail::Fail(
                "interaction.lowering_failed",
                "segment semantic gate wait could not be lowered");
        }

        if (segment.held_through_successor)
        {
            const std::array successor_points{
                *segment.held_through_successor};
            const auto successor_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::StopGroupStaticConfig,
                StopGroupConfig(successor_points),
                "segment/" + segment.canonical_id +
                    "/held-successor/static-config",
                segment_scope);
            const auto successor_request = successor_config
                ? AddRequest(
                      builder,
                      function,
                      block,
                      CanonicalAction::StopPointsSubscribeGroup,
                      std::array{*successor_config},
                      "segment/" + segment.canonical_id +
                          "/held-successor/request",
                      segment_scope)
                : std::nullopt;
            if (!successor_request)
            {
                return detail::Fail(
                    "interaction.lowering_failed",
                    "held-through semantic successor request could not be constructed");
            }
            const auto successor_subscription =
                builder.AddInstruction(
                    function,
                    block,
                    InstructionOpcode::AwaitAction,
                    definition.subscription_type,
                    std::array{*successor_request},
                    ActionTarget(
                        definition.actions.subscribe_group),
                    "segment/" + segment.canonical_id +
                        "/held-successor/" +
                        segment.held_through_successor
                            ->canonical_id,
                    std::nullopt,
                    segment_scope);
            const auto successor_wait_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::ExecutionContinueUntil,
                std::array{
                    *successor_subscription,
                    *wait_publication,
                    *wait_config},
                "segment/" + segment.canonical_id +
                    "/held-successor/continue/request",
                segment_scope);
            stop = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                definition.point_receipt_type,
                std::array{*successor_wait_request},
                ActionTarget(
                    definition.actions.continue_until),
                "segment/" + segment.canonical_id +
                    "/held-through-semantic-successor/" +
                    segment.held_through_successor
                        ->canonical_id,
                std::nullopt,
                {});
            if (!stop)
            {
                return detail::Fail(
                    "interaction.lowering_failed",
                    "held-through semantic successor wait could not be lowered");
            }
        }

        std::optional<ProgramValueId> request_poll;
        if (segment.acknowledgement !=
            InputAcknowledgementPolicy::NotRequired)
        {
            const auto request_publication = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt,
                publication,
                "segment/" + segment.canonical_id +
                    "/request-poll/publication",
                segment_scope);
            const auto no_neutral = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness,
                std::nullopt,
                "segment/" + segment.canonical_id +
                    "/request-poll/no-neutral",
                segment_scope);
            const auto request_poll_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    InputPollStaticConfig,
                PollConfig(
                    false,
                    segment.canonical_id +
                        ".request",
                    1),
                "segment/" + segment.canonical_id +
                    "/request-poll/static-config",
                segment_scope);
            const auto request_poll_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::InputAwaitGuestPoll,
                std::array{
                    *lease,
                    *request_publication,
                    *no_neutral,
                    *request_poll_config},
                "segment/" + segment.canonical_id +
                    "/request-poll/request",
                segment_scope);
            request_poll = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                definition.input_poll_receipt_type,
                std::array{*request_poll_request},
                ActionTarget(definition.actions.await_guest_poll),
                "segment/" + segment.canonical_id +
                    "/request-receipt-before-neutral",
                std::nullopt,
                {});
        }

        for (const auto observation : segment.attached_observations)
        {
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::CallLocal,
                std::nullopt,
                std::array{*stop},
                {
                    .kind = InstructionTargetKind::LocalFunction,
                    .local_function = observation,
                },
                "segment/" + segment.canonical_id +
                    "/ordered-observation",
                std::nullopt,
                segment_scope);
        }
        for (const auto& check : segment.attached_checks)
        {
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::CallReducer,
                TypeRef::Builtin(BuiltinType::Bool),
                std::array{*stop},
                ReducerTarget(check),
                "segment/" + segment.canonical_id + "/check",
                std::nullopt,
                segment_scope);
        }

        std::optional<ProgramValueId> memory_baseline;
        if (segment.memory_change_observation)
        {
            const auto memory_action =
                *FindCanonicalAction(
                    *segment.memory_change_observation);
            AddCanonicalActionSchemaImports(
                builder,
                memory_action);
            const auto memory_request =
                AddMemoryReadRequest(
                    builder,
                    function,
                    block,
                    memory_action,
                    *stop,
                    segment.memory_change_address,
                    "segment/" +
                        segment.canonical_id +
                        "/memory-baseline",
                    segment_scope);
            memory_baseline = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                segment.memory_change_value_type,
                std::array{*memory_request},
                ActionTarget(*segment.memory_change_observation),
                "segment/" + segment.canonical_id +
                    "/memory-baseline-before-advance",
                std::nullopt,
                {});
        }

        const auto neutral_config = AddStaticConfig(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::InputNeutralStaticConfig,
            NeutralConfig(
                segment.acknowledgement ==
                InputAcknowledgementPolicy::
                    RequestAndRelease),
            "segment/" + segment.canonical_id +
                "/neutral/static-config",
            segment_scope);
        const auto neutral_request = AddRequest(
            builder,
            function,
            block,
            CanonicalAction::InputNeutralize,
            std::array{*lease, *neutral_config},
            "segment/" + segment.canonical_id +
                "/neutral/request",
            segment_scope);
        const auto neutral = builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            definition.input_neutral_witness_type,
            std::array{*neutral_request},
            ActionTarget(definition.actions.neutralize),
            "segment/" + segment.canonical_id + "/publish-neutral",
            std::nullopt,
            {});

        if (segment.memory_change_observation)
        {
            const auto one = ConstantU64(
                builder,
                function,
                block,
                1,
                "segment/" + segment.canonical_id +
                    "/one-neutral-frame-between-polls",
                segment_scope);
            const auto no_publication = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness,
                neutral,
                "segment/" + segment.canonical_id +
                    "/neutral-frame/witness",
                segment_scope);
            const auto frame_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    ExecutionAdvanceStaticConfig,
                AdvanceConfig(segment),
                "segment/" + segment.canonical_id +
                    "/neutral-frame/static-config",
                segment_scope);
            const auto frame_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::ExecutionStepFrames,
                std::array{
                    *one,
                    *no_publication,
                    *frame_config},
                "segment/" + segment.canonical_id +
                    "/neutral-frame/request",
                segment_scope);
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                CanonicalActionOutputType(
                    CanonicalAction::
                        ExecutionStepFrames),
                std::array{*frame_request},
                ActionTarget(definition.actions.step_frames),
                std::format(
                    "segment/{}/neutral-frame/poll-bound={}",
                    segment.canonical_id,
                    segment.maximum_memory_polls),
                std::nullopt,
                {});
            const auto memory_action =
                *FindCanonicalAction(
                    *segment.memory_change_observation);
            const auto after_request =
                AddMemoryReadRequest(
                    builder,
                    function,
                    block,
                    memory_action,
                    *stop,
                    segment.memory_change_address,
                    "segment/" +
                        segment.canonical_id +
                        "/memory-after-neutral",
                    segment_scope);
            const auto after = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                segment.memory_change_value_type,
                std::array{*after_request},
                ActionTarget(*segment.memory_change_observation),
                "segment/" + segment.canonical_id +
                    "/memory-observation-after-neutral-frame",
                std::nullopt,
                {});
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::NotEqual,
                TypeRef::Builtin(BuiltinType::Bool),
                std::array{*memory_baseline, *after},
                {},
                "segment/" + segment.canonical_id +
                    "/memory-change-check",
                std::nullopt,
                segment_scope);
        }

        std::optional<ProgramValueId> release_poll;
        if (segment.acknowledgement ==
            InputAcknowledgementPolicy::RequestAndRelease)
        {
            const auto no_publication = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt,
                std::nullopt,
                "segment/" + segment.canonical_id +
                    "/release-poll/no-publication",
                segment_scope);
            const auto neutral_witness = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness,
                neutral,
                "segment/" + segment.canonical_id +
                    "/release-poll/neutral-witness",
                segment_scope);
            const auto release_poll_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    InputPollStaticConfig,
                PollConfig(
                    true,
                    *segment.release_witness_point,
                    1),
                "segment/" + segment.canonical_id +
                    "/release-poll/static-config",
                segment_scope);
            const auto release_poll_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::InputAwaitGuestPoll,
                std::array{
                    *lease,
                    *no_publication,
                    *neutral_witness,
                    *release_poll_config},
                "segment/" + segment.canonical_id +
                    "/release-poll/request",
                segment_scope);
            release_poll = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                definition.input_poll_receipt_type,
                std::array{*release_poll_request},
                ActionTarget(definition.actions.await_guest_poll),
                "segment/" + segment.canonical_id +
                    "/release-witness/" + *segment.release_witness_point,
                std::nullopt,
                {});
        }

        std::vector<ProgramValueId> completion_operands{
            current_state,
            *stop,
            *publication,
            *neutral,
        };
        if (request_poll) completion_operands.push_back(*request_poll);
        if (release_poll) completion_operands.push_back(*release_poll);
        const auto segment_result = builder.AddInstruction(
            function,
            block,
            InstructionOpcode::CallReducer,
            definition.segment_result_type,
            completion_operands,
            ReducerTarget(segment.completion_mapper),
            "segment/" + segment.canonical_id + "/typed-result",
            std::nullopt,
            segment_scope);

        (void)builder.AddInstruction(
            function,
            block,
            InstructionOpcode::ExitScope,
            std::nullopt,
            {},
            {},
            "segment/" + segment.canonical_id + "/release-nested-scope",
            std::nullopt,
            segment_scope);

        if (definition.advance_reducer)
        {
            const auto transition = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::CallReducer,
                definition.adaptive_transition_type,
                std::array{current_state, *segment_result},
                ReducerTarget(*definition.advance_reducer),
                "adaptive/reduce-state-and-select-known-segment",
                std::nullopt,
                outer_scope);
            const auto next_state = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::RecordProject,
                definition.state_type,
                std::array{*transition},
                {},
                definition.adaptive_state_field,
                std::nullopt,
                outer_scope);
            const auto selection = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::RecordProject,
                definition.adaptive_segment_id_type,
                std::array{*transition},
                {},
                definition.adaptive_segment_field,
                std::nullopt,
                outer_scope);
            Terminator terminal{
                .kind = TerminatorKind::EnumSwitch,
                .condition_or_selector = selection,
            };
            for (std::size_t index = 0;
                 index < definition.segments.size();
                 ++index)
            {
                terminal.enum_cases.push_back({
                    .enum_value = static_cast<std::int64_t>(index),
                    .edge = {
                        .target = segment_blocks.at(
                            definition.segments[index].canonical_id),
                        .arguments = {*next_state},
                    },
                });
            }
            terminal.enum_cases.push_back({
                .enum_value =
                    static_cast<std::int64_t>(definition.segments.size()),
                .edge = {
                    .target = complete.id,
                    .arguments = {*next_state, *segment_result},
                },
            });
            builder.SetTerminator(
                function,
                block,
                std::move(terminal),
                "adaptive/dispatch-known-segment");
        }
        else if (segment.static_next_segment)
        {
            builder.SetTerminator(
                function,
                block,
                Terminator{
                    .kind = TerminatorKind::Branch,
                    .edges = {{
                        .target = segment_blocks.at(
                            *segment.static_next_segment),
                        .arguments = {current_state},
                    }},
                },
                "static/next-segment");
        }
        else
        {
            builder.SetTerminator(
                function,
                block,
                Terminator{
                    .kind = TerminatorKind::Branch,
                    .edges = {{
                        .target = complete.id,
                        .arguments = {current_state, *segment_result},
                    }},
                },
                "static/complete");
        }
    }

    const auto output = builder.AddInstruction(
        function,
        complete,
        InstructionOpcode::CallReducer,
        definition.output_type,
        std::array{
            final_state_argument.id,
            final_result_argument.id,
        },
        ReducerTarget(definition.finalize_reducer),
        "reducer/finalize",
        std::nullopt,
        outer_scope);
    (void)builder.AddInstruction(
        function,
        complete,
        InstructionOpcode::ExitScope,
        std::nullopt,
        {},
        {},
        "scope/input-lease/release",
        std::nullopt,
        outer_scope);
    builder.SetTerminator(
        function,
        complete,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = output,
        },
        "return");

    candidate.budgets.maximum_instructions = std::max(
        candidate.budgets.maximum_instructions,
        definition.budgets.maximum_instructions);
    candidate.budgets.maximum_action_requests = std::max(
        candidate.budgets.maximum_action_requests,
        definition.budgets.maximum_action_requests);
    candidate.budgets.maximum_emissions = std::max(
        candidate.budgets.maximum_emissions,
        definition.budgets.maximum_emissions);
    const auto function_id = function.id;
    module = std::move(candidate);
    return {
        .ok = true,
        .function = function_id,
    };
}

} // namespace savor::runtime::program::composition
