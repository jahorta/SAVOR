#include "InteractionComposition.h"

#include "Runner/Runtime/Execution/ExecutionTypes.h"
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

std::vector<Byte> ContinueConfig(
    const InteractionSegmentDefinition& segment)
{
    StaticConfigWriter writer({'C', 'U', 'C', '2'});
    writer.U8(1); // Ignore the current retained point.
    writer.Bool(segment.fail_on_movie_end);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0); // reject unknown interruption
    return std::move(writer).Finish();
}

std::vector<Byte> AdvanceConfig(
    const InteractionSegmentDefinition& segment)
{
    StaticConfigWriter writer({'E', 'A', 'C', '1'});
    writer.U8(2u);
    writer.Bool(segment.fail_on_movie_end);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> LeaseConfig()
{
    StaticConfigWriter writer({'I', 'L', 'C', '2'});
    writer.U32(0); // port
    writer.U32(0); // priority
    writer.Bool(true); // suspendable
    writer.Bool(true); // interruption-borrowable
    writer.Bool(false); // movie-exclusive
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
    std::optional<ProgramValueId> stop,
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
        actions.apply_input_state.canonical_id,
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
        .apply_input_state = CanonicalActionIdentity(
            CanonicalAction::InputApplyState),
        .continue_until = CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntil),
        .step_frames = CanonicalActionIdentity(
            CanonicalAction::ExecutionStepFrames),
    };
    if (actions != canonical_actions ||
        definition.lease_type != CanonicalActionOutputType(
            CanonicalAction::InputAcquireLease) ||
        definition.point_receipt_type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil) ||
        definition.input_execution_binding_type !=
            CanonicalActionOutputType(
                CanonicalAction::InputApplyState))
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
        const TypeRef expected_input = CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload);
        if (segment.input_kind !=
                InteractionInputKind::Neutral &&
            requested_type != expected_input)
        {
            return detail::Fail(
                "interaction.input_type_mismatch",
                "held inputs require InputFramePayload");
        }
        if (segment.input_kind == InteractionInputKind::Neutral &&
            segment.held_through_successor)
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
        if (segment.post_release_gate &&
            (segment.input_kind != InteractionInputKind::Held ||
             segment.post_release_gate->canonical_id.empty() ||
             (segment.post_release_gate->kind ==
                  SemanticPointKind::ProgramCounter &&
              segment.post_release_gate->physical_pc == 0)))
        {
            return detail::Fail(
                "interaction.invalid_post_release_gate",
                "post-release gates require a held input with exact release acknowledgement");
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
        if (!definition.adaptive_selection ||
            definition.adaptive_selection->segments.size() !=
                definition.segments.size() ||
            definition.adaptive_selection->segments.contains(
                definition.adaptive_selection->complete))
        {
            return detail::Fail(
                "interaction.invalid_adaptive_selection",
                "adaptive interactions require one explicit enum mapping per segment and a distinct completion value");
        }
        std::set<std::string> mapped_segments;
        for (const auto& [selection, segment_id] :
             definition.adaptive_selection->segments)
        {
            (void)selection;
            if (!segment_ids.contains(segment_id) ||
                !mapped_segments.insert(segment_id).second)
            {
                return detail::Fail(
                    "interaction.invalid_adaptive_selection",
                    "adaptive enum mappings must name every declared segment exactly once");
            }
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
    else if (definition.adaptive_selection)
    {
        return detail::Fail(
            "interaction.unused_adaptive_selection",
            "static interactions cannot declare an adaptive enum mapping");
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

void AddActionImports(
    ModuleFragmentBuilder& builder,
    const InteractionActionSet& actions)
{
    builder.AddActionImport(actions.acquire_input_lease);
    builder.AddActionImport(actions.apply_input_state);
    builder.AddActionImport(actions.continue_until);
    builder.AddActionImport(actions.step_frames);
    for (const auto action : {
             CanonicalAction::InputAcquireLease,
             CanonicalAction::InputApplyState,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::ExecutionStepFrames,
         })
    {
        AddCanonicalActionSchemaImports(builder, action);
    }
}

std::optional<ProgramValueId> NeutralInputFrame(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
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
            .payload = std::vector<Byte>{0, 0, 128, 128, 128, 128, 0, 0}},
        scope);
}

std::optional<ProgramFunctionId> AddMemoryChangePollFunction(
    ModuleFragmentBuilder& builder,
    const InteractionDefinition& definition,
    const InteractionSegmentDefinition& segment)
{
    if (!segment.memory_change_observation)
        return std::nullopt;

    const auto baseline =
        builder.NewArgument(segment.memory_change_value_type);
    const auto binding =
        builder.NewArgument(definition.input_execution_binding_type);
    const std::array arguments{baseline, binding};
    auto& function = builder.AddFunction(
        "interaction.memory-change." + segment.canonical_id,
        arguments,
        TypeRef::Builtin(BuiltinType::Unit));
    function.blocks.reserve(5);
    auto& entry = builder.AddBlock(function);
    const auto poll_count =
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U64));
    auto& poll = builder.AddBlock(function, std::array{poll_count});
    const auto retry_count =
        builder.NewArgument(TypeRef::Builtin(BuiltinType::U64));
    auto& retry = builder.AddBlock(function, std::array{retry_count});
    auto& changed = builder.AddBlock(function);
    auto& exhausted = builder.AddBlock(function);

    const auto zero = ConstantU64(
        builder,
        function,
        entry,
        0,
        "memory-change/initial-poll-count",
        {});
    if (!zero)
        return std::nullopt;
    builder.SetTerminator(
        function,
        entry,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = poll.id,
                .arguments = {*zero},
            }},
        },
        "memory-change/start");

    const auto one = ConstantU64(
        builder,
        function,
        poll,
        1,
        "memory-change/one-held-frame",
        {});
    const auto input_binding = AddOptional(
        builder,
        function,
        poll,
        CanonicalRuntimeSchema::OptionalInputExecutionBinding,
        binding.id,
        "memory-change/input-binding",
        {});
    const auto frame_config = AddStaticConfig(
        builder,
        function,
        poll,
        CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig,
        AdvanceConfig(segment),
        "memory-change/frame/static-config",
        {});
    const auto frame_request = one && input_binding && frame_config
        ? AddRequest(
              builder,
              function,
              poll,
              CanonicalAction::ExecutionStepFrames,
              std::array{*one, *input_binding, *frame_config},
              "memory-change/frame/request",
              {})
        : std::nullopt;
    if (!frame_request)
        return std::nullopt;
    (void)builder.AddInstruction(
        function,
        poll,
        InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(CanonicalAction::ExecutionStepFrames),
        std::array{*frame_request},
        ActionTarget(definition.actions.step_frames),
        "memory-change/advance-one-held-frame");

    const auto memory_action =
        *FindCanonicalAction(*segment.memory_change_observation);
    const auto memory_request = AddMemoryReadRequest(
        builder,
        function,
        poll,
        memory_action,
        std::nullopt,
        segment.memory_change_address,
        "memory-change/read-after-frame",
        {});
    if (!memory_request)
        return std::nullopt;
    const auto observed = builder.AddInstruction(
        function,
        poll,
        InstructionOpcode::AwaitAction,
        segment.memory_change_value_type,
        std::array{*memory_request},
        ActionTarget(*segment.memory_change_observation),
        "memory-change/observe-after-frame");
    const auto did_change = observed
        ? builder.AddInstruction(
              function,
              poll,
              InstructionOpcode::NotEqual,
              TypeRef::Builtin(BuiltinType::Bool),
              std::array{baseline.id, *observed},
              {},
              "memory-change/compare")
        : std::nullopt;
    const auto next_count = builder.AddInstruction(
        function,
        poll,
        InstructionOpcode::AddChecked,
        TypeRef::Builtin(BuiltinType::U64),
        std::array{poll_count.id, *one},
        {},
        "memory-change/increment-poll-count");
    if (!did_change || !next_count)
        return std::nullopt;
    builder.SetTerminator(
        function,
        poll,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = did_change,
            .edges = {
                {.target = changed.id},
                {
                    .target = retry.id,
                    .arguments = {*next_count},
                },
            },
        },
        "memory-change/changed-or-retry");

    const auto limit = ConstantU64(
        builder,
        function,
        retry,
        segment.maximum_memory_polls,
        "memory-change/poll-limit",
        {});
    const auto may_retry = limit
        ? builder.AddInstruction(
              function,
              retry,
              InstructionOpcode::Less,
              TypeRef::Builtin(BuiltinType::Bool),
              std::array{retry_count.id, *limit},
              {},
              "memory-change/bound-check")
        : std::nullopt;
    if (!may_retry)
        return std::nullopt;
    builder.SetTerminator(
        function,
        retry,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = may_retry,
            .edges = {
                {
                    .target = poll.id,
                    .arguments = {retry_count.id},
                },
                {.target = exhausted.id},
            },
        },
        "memory-change/retry-within-bound");

    builder.SetTerminator(
        function,
        changed,
        Terminator{.kind = TerminatorKind::Return},
        "memory-change/complete");
    builder.SetTerminator(
        function,
        exhausted,
        Terminator{
            .kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{
                "interaction_memory_change_timeout",
                std::format(
                    "Interaction segment '{}' did not observe memory change within {} held frames",
                    segment.canonical_id,
                    segment.maximum_memory_polls),
            },
        },
        "memory-change/exhausted");
    return function.id;
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
             definition.point_receipt_type,
             definition.input_execution_binding_type,
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
        if (segment.post_release_gate)
            builder.AddCapabilityImport(
                segment.post_release_gate->capability_pack);
        builder.AddReducerImport(segment.completion_mapper);
        for (const auto& check : segment.attached_checks)
            builder.AddReducerImport(check);
        if (segment.memory_change_observation)
            builder.AddActionImport(*segment.memory_change_observation);
    }

    std::map<std::string, ProgramFunctionId> memory_poll_functions;
    for (const auto& segment : definition.segments)
    {
        if (!segment.memory_change_observation)
            continue;
        const auto function = AddMemoryChangePollFunction(
            builder,
            definition,
            segment);
        if (!function)
        {
            return detail::Fail(
                "interaction.lowering_failed",
                "bounded memory-change synchronization could not be lowered");
        }
        memory_poll_functions.emplace(segment.canonical_id, *function);
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

    const auto lease_config = AddStaticConfig(
        builder,
        function,
        entry,
        CanonicalRuntimeSchema::InputLeaseStaticConfig,
        LeaseConfig(),
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
            CanonicalRuntimeSchema::SemanticPointSet,
            EncodeSemanticPointSetV1(segment.gate_alternatives),
            "segment/" + segment.canonical_id +
                "/gate/static-config",
            segment_scope);
        if (!gate_config)
            return detail::Fail(
                "interaction.lowering_failed",
                "segment semantic point set could not be constructed");
        std::optional<ProgramValueId> memory_baseline;
        if (segment.memory_change_observation)
        {
            const auto memory_action = *FindCanonicalAction(
                *segment.memory_change_observation);
            AddCanonicalActionSchemaImports(builder, memory_action);
            const auto memory_request = AddMemoryReadRequest(
                builder,
                function,
                block,
                memory_action,
                std::nullopt,
                segment.memory_change_address,
                "segment/" + segment.canonical_id +
                    "/memory-baseline-before-input",
                segment_scope);
            memory_baseline = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                segment.memory_change_value_type,
                std::array{*memory_request},
                ActionTarget(*segment.memory_change_observation),
                "segment/" + segment.canonical_id +
                    "/memory-baseline-before-input",
                std::nullopt,
                {});
        }
        const auto requested_input =
            segment.input_kind == InteractionInputKind::Held
            ? std::optional<ProgramValueId>(
                  arguments[segment.requested_input_parameter].id)
            : NeutralInputFrame(
                  builder,
                  function,
                  block,
                  "segment/" + segment.canonical_id + "/neutral-state",
                  segment_scope);
        if (!requested_input)
            return detail::Fail(
                "interaction.lowering_failed",
                "segment input state could not be constructed");
        const auto apply_request = AddRequest(
            builder,
            function,
            block,
            CanonicalAction::InputApplyState,
            std::array{*lease, *requested_input},
            "segment/" + segment.canonical_id + "/apply-state/request",
            segment_scope);
        const auto binding = apply_request
            ? builder.AddInstruction(
                  function,
                  block,
                  InstructionOpcode::AwaitAction,
                  definition.input_execution_binding_type,
                  std::array{*apply_request},
                  ActionTarget(definition.actions.apply_input_state),
                  "segment/" + segment.canonical_id +
                      "/apply-state-before-departure",
                  std::nullopt,
                  {})
            : std::nullopt;
        if (!binding)
            return detail::Fail(
                "interaction.lowering_failed",
                "segment input state action could not be lowered");
        const auto wait_binding = AddOptional(
            builder,
            function,
            block,
            CanonicalRuntimeSchema::OptionalInputExecutionBinding,
            binding,
            "segment/" + segment.canonical_id +
                "/continue/input-binding",
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
                *gate_config,
                *wait_binding,
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
                CanonicalRuntimeSchema::SemanticPointSet,
                EncodeSemanticPointSetV1(successor_points),
                "segment/" + segment.canonical_id +
                    "/held-successor/static-config",
                segment_scope);
            if (!successor_config)
            {
                return detail::Fail(
                    "interaction.lowering_failed",
                    "held-through semantic successor point set could not be constructed");
            }
            const auto successor_wait_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::ExecutionContinueUntil,
                std::array{
                    *successor_config,
                    *wait_binding,
                    *no_movie,
                    *no_expected_count,
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

        if (segment.memory_change_observation)
        {
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::CallLocal,
                std::nullopt,
                std::array{*memory_baseline, *binding},
                {
                    .kind = InstructionTargetKind::LocalFunction,
                    .local_function =
                        memory_poll_functions.at(segment.canonical_id),
                },
                "segment/" + segment.canonical_id +
                    "/bounded-memory-change-while-state-held",
                std::nullopt,
                segment_scope);
        }

        std::optional<ProgramValueId> neutral_binding = binding;
        if (segment.input_kind == InteractionInputKind::Held)
        {
            const auto neutral_frame = NeutralInputFrame(
                builder,
                function,
                block,
                "segment/" + segment.canonical_id + "/release/frame",
                segment_scope);
            const auto neutral_request = neutral_frame
                ? AddRequest(
                      builder,
                      function,
                      block,
                      CanonicalAction::InputApplyState,
                      std::array{*lease, *neutral_frame},
                      "segment/" + segment.canonical_id +
                          "/release/request",
                      segment_scope)
                : std::nullopt;
            neutral_binding = neutral_request
                ? builder.AddInstruction(
                      function,
                      block,
                      InstructionOpcode::AwaitAction,
                      definition.input_execution_binding_type,
                      std::array{*neutral_request},
                      ActionTarget(definition.actions.apply_input_state),
                      "segment/" + segment.canonical_id +
                          "/release-to-neutral",
                      std::nullopt,
                      {})
                : std::nullopt;
            if (!neutral_binding)
                return detail::Fail(
                    "interaction.lowering_failed",
                    "held segment neutral release could not be lowered");
        }

        if (segment.post_release_gate)
        {
            const std::array post_points{*segment.post_release_gate};
            const auto post_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::SemanticPointSet,
                EncodeSemanticPointSetV1(post_points),
                "segment/" + segment.canonical_id +
                    "/post-release-gate/static-config",
                segment_scope);
            const auto release_binding = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::OptionalInputExecutionBinding,
                neutral_binding,
                "segment/" + segment.canonical_id +
                    "/post-release-gate/input-binding",
                segment_scope);
            const auto post_wait_request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::ExecutionContinueUntil,
                std::array{
                    *post_config,
                    *release_binding,
                    *no_movie,
                    *no_expected_count,
                    *wait_config},
                "segment/" + segment.canonical_id +
                    "/post-release-gate/continue-request",
                segment_scope);
            stop = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                definition.point_receipt_type,
                std::array{*post_wait_request},
                ActionTarget(definition.actions.continue_until),
                "segment/" + segment.canonical_id +
                    "/post-release-gate/" +
                    segment.post_release_gate->canonical_id,
                std::nullopt,
                {});
        }

        if (segment.post_gate_neutral_frames != 0)
        {
            const auto count = ConstantU64(
                builder,
                function,
                block,
                segment.post_gate_neutral_frames,
                "segment/" + segment.canonical_id +
                    "/post-gate-neutral/count",
                segment_scope);
            const auto input_binding = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::OptionalInputExecutionBinding,
                neutral_binding,
                "segment/" + segment.canonical_id +
                    "/post-gate-neutral/input-binding",
                segment_scope);
            const auto frame_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig,
                AdvanceConfig(segment),
                "segment/" + segment.canonical_id +
                    "/post-gate-neutral/static-config",
                segment_scope);
            const auto request = AddRequest(
                builder,
                function,
                block,
                CanonicalAction::ExecutionStepFrames,
                std::array{*count, *input_binding, *frame_config},
                "segment/" + segment.canonical_id +
                    "/post-gate-neutral/request",
                segment_scope);
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                CanonicalActionOutputType(CanonicalAction::ExecutionStepFrames),
                std::array{*request},
                ActionTarget(definition.actions.step_frames),
                "segment/" + segment.canonical_id +
                    "/post-gate-neutral/exact-frames",
                std::nullopt,
                {});
        }

        if (segment.input_kind == InteractionInputKind::Held &&
            !segment.post_release_gate &&
            segment.post_gate_neutral_frames == 0)
        {
            const auto one = ConstantU64(
                builder,
                function,
                block,
                1,
                "segment/" + segment.canonical_id + "/release/one-frame",
                segment_scope);
            const auto input_binding = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::OptionalInputExecutionBinding,
                neutral_binding,
                "segment/" + segment.canonical_id +
                    "/release/input-binding",
                segment_scope);
            const auto frame_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig,
                AdvanceConfig(segment),
                "segment/" + segment.canonical_id +
                    "/release/static-config",
                segment_scope);
            const auto release_request = one && input_binding && frame_config
                ? AddRequest(
                      builder,
                      function,
                      block,
                      CanonicalAction::ExecutionStepFrames,
                      std::array{*one, *input_binding, *frame_config},
                      "segment/" + segment.canonical_id +
                          "/release/request",
                      segment_scope)
                : std::nullopt;
            if (!release_request || !builder.AddInstruction(
                    function,
                    block,
                    InstructionOpcode::AwaitAction,
                    CanonicalActionOutputType(
                        CanonicalAction::ExecutionStepFrames),
                    std::array{*release_request},
                    ActionTarget(definition.actions.step_frames),
                    "segment/" + segment.canonical_id +
                        "/release/observed",
                    std::nullopt,
                    {}))
            {
                return detail::Fail(
                    "interaction.lowering_failed",
                    "held segment release observation could not be lowered");
            }
        }

        const std::array completion_operands{current_state, *stop};
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
            for (const auto& [selection_value, segment_id] :
                 definition.adaptive_selection->segments)
            {
                terminal.enum_cases.push_back({
                    .enum_value = selection_value,
                    .edge = {
                        .target = segment_blocks.at(segment_id),
                        .arguments = {*next_state},
                    },
                });
            }
            terminal.enum_cases.push_back({
                .enum_value = definition.adaptive_selection->complete,
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
