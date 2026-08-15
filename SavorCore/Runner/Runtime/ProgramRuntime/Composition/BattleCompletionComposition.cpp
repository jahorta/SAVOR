#include "BattleCompletionComposition.h"

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace savor::runtime::program::composition {
namespace {

using Builder = detail::ModuleFragmentBuilder;

constexpr std::uint32_t kFastPreseedPc = 0x80101894u;
constexpr std::uint32_t kDeferredPreseedPc = 0x801018acu;
constexpr std::uint32_t kFastPreseedFlag = 0x803475d4u;
constexpr std::uint32_t kArea = 0x80311ac4u;
constexpr std::uint32_t kRawSuffix = 0x80311ac8u;
constexpr std::uint32_t kArea99Suffix = 0x80310a22u;
constexpr std::uint32_t kRng = 0x803469a8u;

class StaticWriter final
{
public:
    explicit StaticWriter(std::array<char, 4> magic)
    {
        for (const auto value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<Byte>(value >> shift));
    }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<Byte> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<Byte> bytes_;
};

std::vector<Byte> ContinueConfig()
{
    StaticWriter writer({'C','U','C','1'});
    writer.U8(1); // FutureOnly
    writer.Bool(true);
    writer.Bool(true);
    writer.U8(0);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> ObservationConfig(std::string_view id)
{
    StaticWriter writer({'O','S','C','1'});
    writer.Text(id);
    writer.U8(0);
    writer.Bool(false);
    writer.U8(0);
    return std::move(writer).Finish();
}

InstructionTarget Action(CanonicalAction action)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = CanonicalActionIdentity(action),
    };
}

InstructionTarget Reducer(const ExactDependencyIdentity& identity)
{
    return {
        .kind = InstructionTargetKind::Reducer,
        .dependency = identity,
    };
}

ProgramValueId Need(
    std::optional<ProgramValueId> value,
    std::string_view operation)
{
    if (!value)
        throw std::logic_error(
            "Battle completion sequence lowering failed: " +
            std::string(operation));
    return *value;
}

BasicBlock& Block(ProgramFunction& function, ProgramBlockId id)
{
    const auto found = std::ranges::find(function.blocks, id, &BasicBlock::id);
    if (found == function.blocks.end())
        throw std::logic_error(
            "Battle completion sequence block is unavailable");
    return *found;
}

void AddAction(Builder& builder, CanonicalAction action)
{
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const auto& schema : CanonicalActionTypeSchemaClosure(action))
        builder.AddTypeImport(schema);
}

ProgramValueId Constant(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    TypeRef type,
    LiteralPayload payload,
    std::string selector)
{
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        type,
        {},
        {},
        std::move(selector),
        LiteralValue{type, std::move(payload)}), "constant");
}

ProgramValueId Construct(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    TypeRef type,
    std::span<const ProgramValueId> fields,
    std::string selector)
{
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordConstruct,
        type,
        fields,
        {},
        std::move(selector)), "record");
}

ProgramValueId Project(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId value,
    TypeRef type,
    std::string field)
{
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordProject,
        type,
        std::array{value},
        {},
        std::move(field)), "projection");
}

ProgramValueId Optional(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::optional<ProgramValueId> value,
    std::string selector)
{
    std::vector<ProgramValueId> operands;
    if (value)
        operands.push_back(*value);
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(schema),
        operands,
        {},
        std::move(selector)), "optional");
}

ProgramValueId ContinueTo(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::span<const SemanticPointReference> points,
    std::string selector)
{
    const auto point_set = Constant(
        builder,
        function,
        block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::SemanticPointSet),
        EncodeSemanticPointSetV1(points),
        selector + "/points");
    const auto no_input = Optional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalInputExecutionBinding,
        std::nullopt,
        selector + "/no-input");
    const auto no_movie = Optional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalMoviePlaybackSession,
        std::nullopt,
        selector + "/no-movie");
    const auto no_count = Optional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalMovieInputCount,
        std::nullopt,
        selector + "/no-count");
    const auto config = Constant(
        builder,
        function,
        block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ContinueUntilStaticConfig),
        ContinueConfig(),
        selector + "/config");
    const std::array fields{point_set, no_input, no_movie, no_count, config};
    const auto request = Construct(
        builder,
        function,
        block,
        CanonicalActionInputType(CanonicalAction::ExecutionContinueUntil),
        fields,
        selector + "/request");
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(CanonicalAction::ExecutionContinueUntil),
        std::array{request},
        Action(CanonicalAction::ExecutionContinueUntil),
        selector + "/wait"), "foreground wait");
}

ProgramValueId ReadScalar(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId stop,
    CanonicalAction action,
    std::uint32_t address,
    std::string selector)
{
    const auto receipt = Optional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalContinueUntilResult,
        stop,
        selector + "/stop");
    const auto location = Constant(
        builder,
        function,
        block,
        TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(address),
        selector + "/address");
    const auto config = Constant(
        builder,
        function,
        block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ObservationStaticConfig),
        ObservationConfig(selector),
        selector + "/config");
    const auto request = Construct(
        builder,
        function,
        block,
        CanonicalActionInputType(action),
        std::array{receipt, location, config},
        selector + "/request");
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(action),
        std::array{request},
        Action(action),
        selector + "/read"), "guest read");
}

ProgramValueId CaptureSnapshot(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId epoch,
    ProgramValueId pc,
    std::string selector)
{
    const auto request = Construct(
        builder,
        function,
        block,
        TypeRef::Named(
            capabilities::BattleCaptureContextRequestSchemaIdentity()),
        std::array{epoch, pc},
        selector + "/request");
    return Need(builder.AddInstruction(
        function,
        block,
        InstructionOpcode::AwaitAction,
        TypeRef::Named(
            capabilities::BattleCompletionSnapshotSchemaIdentity()),
        std::array{request},
        {
            .kind = InstructionTargetKind::Action,
            .dependency = capabilities::
                BattleCompletionCaptureSnapshotActionIdentity(),
        },
        selector + "/capture"), "completion snapshot");
}

SemanticPointReference Point(std::string id, std::uint32_t pc)
{
    return {
        .capability_pack = capabilities::BattleCompletionPackIdentity(),
        .canonical_id = std::move(id),
        .kind = SemanticPointKind::ProgramCounter,
        .physical_pc = pc,
    };
}

InteractionSegmentDefinition NeutralSegment(
    std::string id,
    std::vector<SemanticPointReference> points)
{
    return {
        .canonical_id = std::move(id),
        .gate_alternatives = std::move(points),
        .requested_input_parameter = 0,
        .input_kind = InteractionInputKind::Neutral,
        .completion_mapper = capabilities::
            BattleCompletionInteractionCompleteSegmentReducerIdentity(),
    };
}

} // namespace

InteractionDefinition BattleCompletionInteractionV1()
{
    const auto input_frame = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
    const auto continue_result = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    InteractionDefinition definition{
        .canonical_id = "soa.battle.completion.interaction",
        .revision = 1,
        .source_name = "BattleCompletionComposition",
        .parameters = {{"neutral_input", input_frame}},
        .state_type = TypeRef::Named(capabilities::
            BattleCompletionInteractionStateSchemaIdentity()),
        .output_type = TypeRef::Named(capabilities::
            BattleCompletionInteractionReceiptSchemaIdentity()),
        .lease_type = CanonicalActionOutputType(
            CanonicalAction::InputAcquireLease),
        .point_receipt_type = continue_result,
        .input_execution_binding_type = CanonicalActionOutputType(
            CanonicalAction::InputApplyState),
        .segment_result_type = continue_result,
        .adaptive_transition_type = TypeRef::Named(capabilities::
            BattleCompletionInteractionTransitionSchemaIdentity()),
        .adaptive_segment_id_type = TypeRef::Named(capabilities::
            BattleCompletionInteractionSegmentSchemaIdentity()),
        .adaptive_selection = InteractionAdaptiveSelectionMap{
            .segments = {
                {0, "causal"},
                {1, "countdown_commit"},
                {2, "slots_commit"},
                {3, "reward_entry"},
                {4, "reward_commit"},
            },
            .complete = 5,
        },
        .initialize_reducer = capabilities::
            BattleCompletionInteractionInitializeReducerIdentity(),
        .advance_reducer = capabilities::
            BattleCompletionInteractionAdvanceReducerIdentity(),
        .finalize_reducer = capabilities::
            BattleCompletionInteractionFinalizeReducerIdentity(),
        .actions = {
            .acquire_input_lease = CanonicalActionIdentity(
                CanonicalAction::InputAcquireLease),
            .apply_input_state = CanonicalActionIdentity(
                CanonicalAction::InputApplyState),
            .continue_until = CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil),
            .step_frames = CanonicalActionIdentity(
                CanonicalAction::ExecutionStepFrames),
        },
        .segments = {
            NeutralSegment("causal", {
                Point("soa.battle.completion.point.VictoryCountdownComplete", 0x8006f554u),
                Point("soa.battle.completion.point.VictorySlotsComplete", 0x8006f590u),
            }),
            NeutralSegment("countdown_commit", {
                Point("soa.battle.completion.point.VictoryCountdownCommitted", 0x8006f558u),
            }),
            NeutralSegment("slots_commit", {
                Point("soa.battle.completion.point.VictorySlotsCommitted", 0x8006f594u),
            }),
            NeutralSegment("reward_entry", {
                Point("soa.battle.completion.point.RewardEntry", 0x8006f598u),
            }),
            NeutralSegment("reward_commit", {
                Point("soa.battle.completion.point.RewardCommit", 0x8006fd58u),
            }),
        },
        .first_segment = "causal",
        .budgets = {
            .maximum_instructions = 4096,
            .maximum_calls = 64,
            .maximum_call_depth = 4,
            .maximum_action_requests = 128,
            .maximum_emissions = 0,
            .maximum_artifacts = 0,
            .maximum_values = 8192,
            .maximum_value_bytes = 8 * 1024 * 1024,
            .maximum_trace_events = 4096,
        },
    };
    return definition;
}

CompositionResult LowerBattleCompletionInteractionV1(ProgramModule& module)
{
    return LowerInteraction(BattleCompletionInteractionV1(), module);
}

SchemaIdentity BattleCompletionSequenceReceiptSchemaIdentity()
{
    return ExactSchema(
        "soa.battle.completion.SequenceReceipt",
        1,
        "record BattleCompletionSequenceReceipt/1(manifest:soa.battle.completion.Manifest/1,transition:soa.field.TransitionContext/1,terminal_movie_input_count:u64)");
}

CompositionResult LowerBattleCompletionSequenceV1(ProgramModule& module)
{
    const auto interaction = LowerBattleCompletionInteractionV1(module);
    if (!interaction || !interaction.function)
        return interaction;

    try
    {
        Builder builder(
            module,
            "BattleCompletionComposition",
            "soa.battle.completion.sequence/v1");
        const auto result_schema =
            BattleCompletionSequenceReceiptSchemaIdentity();
        builder.AddLocalType({
            .identity = result_schema,
            .kind = TypeSchemaKind::Record,
            .record_fields = {
                {"manifest", TypeRef::Named(
                    capabilities::BattleCompletionManifestSchemaIdentity())},
                {"transition", TypeRef::Named(
                    capabilities::FieldTransitionContextSchemaIdentity())},
                {"terminal_movie_input_count",
                    TypeRef::Builtin(BuiltinType::U64)},
            },
        });

        builder.AddCapabilityImport(
            capabilities::BattleCompletionPackIdentity());
        builder.AddActionImport(
            capabilities::BattleCompletionCaptureSnapshotActionIdentity());
        builder.AddReducerImport(
            capabilities::BattleCompletionBuildManifestReducerIdentity());
        builder.AddReducerImport(
            capabilities::FieldTransitionBuildContextReducerIdentity());
        for (const auto& schema : {
                 capabilities::BattleCaptureContextRequestSchemaIdentity(),
                 capabilities::BattleCompletionSnapshotSchemaIdentity(),
                 capabilities::BattleCompletionManifestSchemaIdentity(),
                 capabilities::FieldTransitionContextSchemaIdentity(),
                 capabilities::BattleCompletionInteractionReceiptSchemaIdentity(),
             })
        {
            builder.AddTypeImport(schema);
        }
        for (const auto action : {
                 CanonicalAction::ExecutionContinueUntil,
                 CanonicalAction::GuestReadU8,
                 CanonicalAction::GuestReadU32,
             })
        {
            AddAction(builder, action);
        }

        const auto victory_pc = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U32));
        const auto victory_vi = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const auto victory_epoch = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const auto neutral_input = builder.NewArgument(
            CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload));
        const auto battle_set_id = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const auto wave_id = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const auto turn_job_id = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const auto execution_job_id = builder.NewArgument(
            TypeRef::Builtin(BuiltinType::U64));
        const std::array arguments{
            victory_pc,
            victory_vi,
            victory_epoch,
            neutral_input,
            battle_set_id,
            wave_id,
            turn_job_id,
            execution_job_id,
        };
        auto& function = builder.AddFunction(
            "soa.battle.completion.sequence.v1",
            arguments,
            TypeRef::Named(result_schema));
        const auto entry_id = builder.AddBlock(function).id;
        auto& entry = Block(function, entry_id);

        const auto before = CaptureSnapshot(
            builder,
            function,
            entry,
            victory_epoch.id,
            victory_pc.id,
            "entry/snapshot");
        const auto interaction_receipt = Need(builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::CallLocal,
            TypeRef::Named(capabilities::
                BattleCompletionInteractionReceiptSchemaIdentity()),
            std::array{neutral_input.id},
            {
                .kind = InstructionTargetKind::LocalFunction,
                .local_function = *interaction.function,
            },
            "completion/adaptive-interaction"),
            "completion interaction");
        const auto reward_entry_pc = Project(
            builder,
            function,
            entry,
            interaction_receipt,
            TypeRef::Builtin(BuiltinType::U32),
            "reward_entry_pc");
        const auto reward_entry_vi = Project(
            builder,
            function,
            entry,
            interaction_receipt,
            TypeRef::Builtin(BuiltinType::U64),
            "reward_entry_vi_count");
        const auto reward_entry_epoch = Project(
            builder,
            function,
            entry,
            interaction_receipt,
            TypeRef::Builtin(BuiltinType::U64),
            "reward_entry_epoch");
        const auto reward_commit = Project(
            builder,
            function,
            entry,
            interaction_receipt,
            CanonicalActionOutputType(CanonicalAction::ExecutionContinueUntil),
            "reward_commit");
        const auto reward_commit_pc = Project(
            builder,
            function,
            entry,
            reward_commit,
            TypeRef::Builtin(BuiltinType::U32),
            "pc");
        const auto reward_commit_vi = Project(
            builder,
            function,
            entry,
            reward_commit,
            TypeRef::Builtin(BuiltinType::U64),
            "vi_count");
        const auto reward_commit_epoch = Project(
            builder,
            function,
            entry,
            reward_commit,
            TypeRef::Builtin(BuiltinType::U64),
            "workset_epoch");
        const auto after = CaptureSnapshot(
            builder,
            function,
            entry,
            reward_commit_epoch,
            reward_commit_pc,
            "reward-commit/snapshot");

        const std::array preseed_points{
            Point(
                "soa.battle.completion.point.FieldPreseedFast",
                kFastPreseedPc),
            Point(
                "soa.battle.completion.point.FieldPreseedDeferred",
                kDeferredPreseedPc),
        };
        const auto first_preseed = ContinueTo(
            builder,
            function,
            entry,
            preseed_points,
            "field-preseed/first");
        const auto first_pc = Project(
            builder,
            function,
            entry,
            first_preseed,
            TypeRef::Builtin(BuiltinType::U32),
            "pc");
        const auto fast_pc = Constant(
            builder,
            function,
            entry,
            TypeRef::Builtin(BuiltinType::U32),
            kFastPreseedPc,
            "field-preseed/fast-pc");
        const auto is_fast = Need(builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{first_pc, fast_pc},
            {},
            "field-preseed/is-fast"), "preseed dispatch");

        const auto stop_argument = builder.NewArgument(
            CanonicalActionOutputType(CanonicalAction::ExecutionContinueUntil));
        const auto accepted_id = builder.AddBlock(
            function,
            std::array{stop_argument}).id;
        const auto fast_id = builder.AddBlock(function).id;
        builder.SetTerminator(
            function,
            Block(function, entry_id),
            {
                .kind = TerminatorKind::ConditionalBranch,
                .condition_or_selector = is_fast,
                .edges = {
                    {.target = fast_id},
                    {.target = accepted_id, .arguments = {first_preseed}},
                },
            },
            "field-preseed/dispatch");

        auto& fast = Block(function, fast_id);
        const auto flag = ReadScalar(
            builder,
            function,
            fast,
            first_preseed,
            CanonicalAction::GuestReadU32,
            kFastPreseedFlag,
            "field-preseed/fast-flag");
        const auto zero = Constant(
            builder,
            function,
            fast,
            TypeRef::Builtin(BuiltinType::U32),
            std::uint32_t{0},
            "field-preseed/zero");
        const auto fast_accepted = Need(builder.AddInstruction(
            function,
            fast,
            InstructionOpcode::NotEqual,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{flag, zero},
            {},
            "field-preseed/fast-accepted"),
            "fast preseed qualification");
        const auto deferred_id = builder.AddBlock(function).id;
        builder.SetTerminator(
            function,
            Block(function, fast_id),
            {
                .kind = TerminatorKind::ConditionalBranch,
                .condition_or_selector = fast_accepted,
                .edges = {
                    {.target = accepted_id, .arguments = {first_preseed}},
                    {.target = deferred_id},
                },
            },
            "field-preseed/fast-qualification");
        const std::array deferred_point{
            Point(
                "soa.battle.completion.point.FieldPreseedDeferred",
                kDeferredPreseedPc),
        };
        auto& deferred = Block(function, deferred_id);
        const auto deferred_stop = ContinueTo(
            builder,
            function,
            deferred,
            deferred_point,
            "field-preseed/deferred");
        builder.SetTerminator(
            function,
            deferred,
            {
                .kind = TerminatorKind::Branch,
                .edges = {{
                    .target = accepted_id,
                    .arguments = {deferred_stop},
                }},
            },
            "field-preseed/deferred-accepted");

        auto& accepted = Block(function, accepted_id);
        const auto preseed = accepted.arguments.front().id;
        const auto area = ReadScalar(
            builder,
            function,
            accepted,
            preseed,
            CanonicalAction::GuestReadU32,
            kArea,
            "transition/area");
        const auto raw_suffix = ReadScalar(
            builder,
            function,
            accepted,
            preseed,
            CanonicalAction::GuestReadU8,
            kRawSuffix,
            "transition/raw-suffix");
        const auto area99_suffix = ReadScalar(
            builder,
            function,
            accepted,
            preseed,
            CanonicalAction::GuestReadU8,
            kArea99Suffix,
            "transition/area99-suffix");
        const auto rng = ReadScalar(
            builder,
            function,
            accepted,
            preseed,
            CanonicalAction::GuestReadU32,
            kRng,
            "transition/rng");
        const auto area99 = Constant(
            builder,
            function,
            accepted,
            TypeRef::Builtin(BuiltinType::U32),
            std::uint32_t{99},
            "transition/area99");
        const auto has_area99 = Need(builder.AddInstruction(
            function,
            accepted,
            InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{area, area99},
            {},
            "transition/uses-area99-suffix"), "area 99 test");
        const auto preseed_pc = Project(
            builder,
            function,
            accepted,
            preseed,
            TypeRef::Builtin(BuiltinType::U32),
            "pc");
        const auto preseed_vi = Project(
            builder,
            function,
            accepted,
            preseed,
            TypeRef::Builtin(BuiltinType::U64),
            "vi_count");
        const auto preseed_epoch = Project(
            builder,
            function,
            accepted,
            preseed,
            TypeRef::Builtin(BuiltinType::U64),
            "workset_epoch");
        const auto terminal_movie_input_count = Project(
            builder,
            function,
            accepted,
            preseed,
            TypeRef::Builtin(BuiltinType::U64),
            "movie_input_count");
        const auto transition = Need(builder.AddInstruction(
            function,
            accepted,
            InstructionOpcode::CallReducer,
            TypeRef::Named(
                capabilities::FieldTransitionContextSchemaIdentity()),
            std::array{
                area,
                raw_suffix,
                area99_suffix,
                has_area99,
                rng,
                preseed_pc,
                preseed_vi,
                preseed_epoch,
            },
            Reducer(
                capabilities::FieldTransitionBuildContextReducerIdentity()),
            "transition/build-context"), "field transition reducer");
        const auto manifest = Need(builder.AddInstruction(
            function,
            accepted,
            InstructionOpcode::CallReducer,
            TypeRef::Named(
                capabilities::BattleCompletionManifestSchemaIdentity()),
            std::array{
                before,
                after,
                victory_pc.id,
                victory_vi.id,
                victory_epoch.id,
                reward_entry_pc,
                reward_entry_vi,
                reward_entry_epoch,
                reward_commit_pc,
                reward_commit_vi,
                reward_commit_epoch,
                transition,
                battle_set_id.id,
                wave_id.id,
                turn_job_id.id,
                execution_job_id.id,
            },
            Reducer(
                capabilities::BattleCompletionBuildManifestReducerIdentity()),
            "completion/build-manifest"), "completion manifest reducer");
        const auto result = Construct(
            builder,
            function,
            accepted,
            TypeRef::Named(result_schema),
            std::array{manifest, transition, terminal_movie_input_count},
            "completion/sequence-result");
        builder.SetTerminator(
            function,
            accepted,
            {
                .kind = TerminatorKind::Return,
                .return_value = result,
            },
            "completion/sequence-return");

        return {
            .ok = true,
            .function = function.id,
        };
    }
    catch (const std::exception& error)
    {
        return detail::Fail(
            "battle_completion_sequence_lowering_failed",
            error.what());
    }
}

} // namespace savor::runtime::program::composition
