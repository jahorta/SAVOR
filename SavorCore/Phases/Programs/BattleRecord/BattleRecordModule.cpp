#include "BattleRecordModule.h"
#include "BattleReplayModule.h"

#include "Runner/Runtime/ProgramKind.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Composition/BattleCompletionComposition.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Composition/InputDeliveryComposition.h"
#include "Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.h"
#include "Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "Runner/Runtime/ProgramRuntime/IR/ProgramModuleSpecialization.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "Runner/Runtime/Services/Savestate/SavestateTypes.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>

namespace savor::runtime::battlerecord {
namespace {

using namespace program;
using namespace program::composition;
using Builder = program::composition::detail::ModuleFragmentBuilder;

constexpr std::uint32_t kBeforeSeed = battlesingleturn::BeforeRandSeedSetPc;
constexpr std::uint32_t kAfterSeed = battlesingleturn::AfterRandSeedSetPc;
constexpr std::uint32_t kTurnInputs = battlesingleturn::TurnInputsPc;
constexpr std::uint32_t kVictory = battlesingleturn::VictoryPc;
constexpr std::uint32_t kDefeat = battlesingleturn::DefeatPc;
constexpr std::uint32_t kRng = battlesingleturn::RngSeedAddress;

enum class PhaseFlavor : std::uint8_t { Record, Replay };

constexpr std::uint32_t kModuleGeneratorContractVersion = 1;

std::string_view ModuleId(PhaseFlavor flavor)
{
    return flavor == PhaseFlavor::Record ? ModuleCanonicalId
        : battlereplay::ModuleCanonicalId;
}

std::string_view EntrypointId(PhaseFlavor flavor)
{
    return flavor == PhaseFlavor::Record ? Entrypoint
        : battlereplay::Entrypoint;
}

std::string SpecializedModuleId(
    const BattleReplayPlanV1& plan,
    PhaseFlavor flavor)
{
    std::vector<ProgramModuleSpecializationFieldV1> fields;
    fields.reserve(2 + plan.turns.size() * 3);
    fields.push_back({"flavor", flavor == PhaseFlavor::Record
        ? "record" : "replay"});
    fields.push_back({"turn_count", std::to_string(plan.turns.size())});
    for (const auto& turn : plan.turns)
    {
        fields.push_back({"turn_index", std::to_string(turn.turn_index)});
        fields.push_back({"expected_outcome", std::to_string(
            static_cast<std::uint32_t>(turn.expected_outcome))});
        fields.push_back({"expected_ending_rng",
            std::to_string(turn.expected_ending_rng)});
    }
    return MakeSpecializedProgramModuleIdV1(
        ModuleId(flavor), kModuleGeneratorContractVersion, fields);
}

void Diagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

SchemaIdentity ExactSchema(std::string id, std::string contract)
{
    SchemaIdentity result{.canonical_id = std::move(id), .version = 1};
    result.schema_hash = ContentHash256::FromHex(
        hash::sha256(contract.data(), contract.size())).value();
    return result;
}

SchemaIdentity RequestSchema(std::size_t turns, PhaseFlavor flavor)
{
    std::string contract = flavor == PhaseFlavor::Record
        ? "record BattleRecordRequest/1(recording,save,seed_frame,expected_manifest,expected_transition,battle_set_id,wave_id,turn_job_id,execution_job_id"
        : "record BattleReplayRequest/1(seed_frame,expected_manifest,expected_transition,battle_set_id,wave_id,turn_job_id,execution_job_id";
    for (std::size_t index = 0; index < turns; ++index)
        contract += ",turn_" + std::to_string(index + 1) + ":soa.battle.BattleTurnExecutionSpec/1";
    contract += ')';
    return ExactSchema(flavor == PhaseFlavor::Record
        ? "soa.battle.record.Request" : "soa.battle.replay.Request",
        std::move(contract));
}

SchemaIdentity OutcomeSchema(PhaseFlavor flavor)
{
    return flavor == PhaseFlavor::Record
        ? ExactSchema("soa.battle.record.Outcome",
            "enum BattleRecordOutcome/1{Recorded=0,ReplayMismatch=1}")
        : ExactSchema("soa.battle.replay.Outcome",
            "enum BattleReplayOutcome/1{Matched=0,ReplayMismatch=1}");
}

SchemaIdentity ResultSchema(PhaseFlavor flavor)
{
    return flavor == PhaseFlavor::Record
        ? ExactSchema("soa.battle.record.Result",
            "record BattleRecordResult/1(outcome:Outcome,mismatch_turn:u32,expected_rng:u32,observed_rng:u32,observed_manifest:optional<soa.battle.completion.Manifest/1>,observed_transition:optional<soa.field.TransitionContext/1>,anchor_input_count:u64,checkpoint_input_count:u64,final_input_count:u64)")
        : ExactSchema("soa.battle.replay.Result",
            "record BattleReplayResult/1(outcome:Outcome,mismatch_turn:u32,expected_rng:u32,observed_rng:u32,observed_manifest:optional<soa.battle.completion.Manifest/1>,observed_transition:optional<soa.field.TransitionContext/1>)");
}

SchemaIdentity OptionalManifestSchema(PhaseFlavor flavor)
{
    return ExactSchema(
        flavor == PhaseFlavor::Record
            ? "soa.battle.record.OptionalObservedManifest"
            : "soa.battle.replay.OptionalObservedManifest",
        "optional<soa.battle.completion.Manifest/1>");
}

SchemaIdentity OptionalTransitionSchema(PhaseFlavor flavor)
{
    return ExactSchema(
        flavor == PhaseFlavor::Record
            ? "soa.battle.record.OptionalObservedTransition"
            : "soa.battle.replay.OptionalObservedTransition",
        "optional<soa.field.TransitionContext/1>");
}

TypeRef RequestType(std::size_t turns, PhaseFlavor flavor)
{
    return TypeRef::Named(RequestSchema(turns, flavor));
}
TypeRef ResultType(PhaseFlavor flavor)
{
    return TypeRef::Named(ResultSchema(flavor));
}

class StaticWriter
{
public:
    explicit StaticWriter(std::array<char, 4> magic)
    {
        for (const auto byte : magic) bytes_.push_back(static_cast<Byte>(byte));
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
    std::vector<Byte> Finish() { return std::move(bytes_); }
private:
    std::vector<Byte> bytes_;
};

std::vector<Byte> ContinueConfig()
{
    StaticWriter writer({'C','U','C','2'});
    writer.U8(1); writer.U8(1);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return writer.Finish();
}

std::vector<Byte> ObservationConfig(std::string_view name)
{
    StaticWriter writer({'O','S','C','1'});
    writer.Text(name); writer.U8(0); writer.Bool(false); writer.U8(0);
    return writer.Finish();
}

std::vector<Byte> RecordingConfig(
    std::string_view path, std::string_view label)
{
    StaticWriter writer({'M','R','C','1'});
    writer.Text(path); writer.Text(label); return writer.Finish();
}

std::vector<Byte> LeaseConfig()
{
    StaticWriter writer({'I','L','C','2'});
    writer.U32(0); writer.U32(0);
    writer.Bool(true); writer.Bool(true); writer.Bool(false);
    return writer.Finish();
}

std::vector<Byte> AdvanceConfig()
{
    StaticWriter writer({'E','A','C','1'});
    writer.U8(2); writer.Bool(false);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return writer.Finish();
}

std::vector<Byte> FrameBytes(const GCInputFrame& frame)
{
    return {static_cast<Byte>(frame.buttons),
            static_cast<Byte>(frame.buttons >> 8u), frame.main_x, frame.main_y,
            frame.c_x, frame.c_y, frame.trig_l, frame.trig_r};
}

InstructionTarget Action(CanonicalAction action)
{
    return {.kind = InstructionTargetKind::Action,
            .dependency = CanonicalActionIdentity(action)};
}
InstructionTarget Reducer(const ExactDependencyIdentity& reducer)
{
    return {.kind = InstructionTargetKind::Reducer, .dependency = reducer};
}
InstructionTarget Reducer(CanonicalReducer reducer)
{
    return Reducer(CanonicalReducerIdentity(reducer));
}

ProgramValueId Need(std::optional<ProgramValueId> value, std::string_view what)
{
    if (!value) throw std::logic_error("battle.record lowering failed: " + std::string(what));
    return *value;
}

void AddAction(Builder& builder, CanonicalAction action)
{
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const auto& schema : CanonicalActionTypeSchemaClosure(action))
        builder.AddTypeImport(schema);
}

ProgramValueId Constant(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    TypeRef type, LiteralPayload payload, std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::Constant, type, {}, {},
        std::move(selector), LiteralValue{type, std::move(payload)}), "constant");
}

ProgramValueId Construct(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    TypeRef type, std::span<const ProgramValueId> fields, std::string selector,
    ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct, type, fields, {},
        std::move(selector), std::nullopt, scope), "record");
}

ProgramValueId Project(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId value, TypeRef type, std::string field)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordProject, type,
        std::array{value}, {}, std::move(field)), "projection");
}

ProgramValueId Optional(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    CanonicalRuntimeSchema schema, std::optional<ProgramValueId> value,
    std::string selector)
{
    std::vector<ProgramValueId> operands;
    if (value) operands.push_back(*value);
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(schema), operands, {}, std::move(selector)),
        "optional");
}

ProgramValueId Await(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    CanonicalAction action, ProgramValueId request, std::string selector,
    ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(action), std::array{request}, Action(action),
        std::move(selector), std::nullopt, scope), "action");
}

ProgramValueId RequirePc(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    std::uint32_t pc, std::string selector)
{
    const auto expected = Constant(
        builder, function, block, TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(pc), selector + "/expected");
    const auto request = Construct(
        builder, function, block,
        CanonicalActionInputType(CanonicalAction::ExecutionRequirePausedPc),
        std::array{expected}, selector + "/request");
    return Await(builder, function, block,
        CanonicalAction::ExecutionRequirePausedPc, request, selector + "/require");
}

SemanticPointReference BattlePoint(std::string name, std::uint32_t pc)
{
    return {.capability_pack = capabilities::BattlePackIdentity(),
            .canonical_id = "soa.battle.point." + std::move(name),
            .kind = SemanticPointKind::ProgramCounter, .physical_pc = pc};
}

ProgramValueId ContinueTo(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    std::span<const SemanticPointReference> points, std::string selector)
{
    const auto point_set = Constant(
        builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::SemanticPointSet),
        EncodeSemanticPointSetV1(points), selector + "/points");
    const auto no_input = Optional(builder, function, block,
        CanonicalRuntimeSchema::OptionalInputExecutionBinding, std::nullopt,
        selector + "/no-input");
    const auto no_movie = Optional(builder, function, block,
        CanonicalRuntimeSchema::OptionalMoviePlaybackSession, std::nullopt,
        selector + "/no-movie");
    const auto no_count = Optional(builder, function, block,
        CanonicalRuntimeSchema::OptionalMovieInputCount, std::nullopt,
        selector + "/no-count");
    const auto config = Constant(builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ContinueUntilStaticConfig),
        ContinueConfig(), selector + "/config");
    const auto one = Constant(builder, function, block,
        TypeRef::Builtin(BuiltinType::U64), std::uint64_t{1},
        selector + "/one-occurrence");
    const auto no_verify = Constant(builder, function, block,
        TypeRef::Builtin(BuiltinType::Bool), false,
        selector + "/no-bound-input-verification");
    const std::array fields{
        point_set, no_input, no_movie, no_count,
        one, no_verify, config};
    const auto request = Construct(builder, function, block,
        CanonicalActionInputType(CanonicalAction::ExecutionContinueUntil),
        fields, selector + "/request");
    return Await(builder, function, block, CanonicalAction::ExecutionContinueUntil,
        request, selector + "/wait");
}

ProgramValueId ReadScalar(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId stop, CanonicalAction action, std::uint32_t address,
    std::string selector)
{
    const auto receipt = Optional(builder, function, block,
        CanonicalRuntimeSchema::OptionalContinueUntilResult, stop,
        selector + "/stop");
    const auto location = Constant(builder, function, block,
        TypeRef::Builtin(BuiltinType::U64), static_cast<std::uint64_t>(address),
        selector + "/address");
    const auto config = Constant(builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ObservationStaticConfig),
        ObservationConfig(selector), selector + "/config");
    const auto request = Construct(builder, function, block,
        CanonicalActionInputType(action),
        std::array{receipt, location, config}, selector + "/request");
    return Await(builder, function, block, action, request, selector + "/read");
}

ProgramValueId CaptureContext(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId stop, std::string selector)
{
    const auto epoch = Project(builder, function, block, stop,
        TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
    const auto pc = Project(builder, function, block, stop,
        TypeRef::Builtin(BuiltinType::U32), "pc");
    const auto request = Construct(builder, function, block,
        TypeRef::Named(capabilities::BattleCaptureContextRequestSchemaIdentity()),
        std::array{epoch, pc}, selector + "/request");
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::AwaitAction,
        TypeRef::Named(capabilities::BattleContextSchemaIdentity()),
        std::array{request}, {.kind = InstructionTargetKind::Action,
            .dependency = capabilities::BattleCaptureContextActionIdentity()},
        selector + "/capture"), "Battle Context capture");
}

ProgramValueId BoolAnd(
    Builder& b, ProgramFunction& f, BasicBlock& block,
    ProgramValueId lhs, ProgramValueId rhs, std::string selector)
{
    return Need(b.AddInstruction(f, block, InstructionOpcode::BooleanAnd,
        TypeRef::Builtin(BuiltinType::Bool), std::array{lhs, rhs}, {},
        std::move(selector)), "BooleanAnd");
}

ProgramValueId Equal(
    Builder& b, ProgramFunction& f, BasicBlock& block,
    ProgramValueId lhs, ProgramValueId rhs, std::string selector)
{
    return Need(b.AddInstruction(f, block, InstructionOpcode::Equal,
        TypeRef::Builtin(BuiltinType::Bool), std::array{lhs, rhs}, {},
        std::move(selector)), "Equal");
}

ProgramValueId Result(
    Builder& b, ProgramFunction& f, BasicBlock& block,
    PhaseFlavor flavor,
    battlecompletion::BattleRecordOutcomeV1 outcome,
    ProgramValueId mismatch_turn, ProgramValueId expected_rng,
    ProgramValueId observed_rng, std::optional<ProgramValueId> manifest,
    std::optional<ProgramValueId> transition, ProgramValueId anchor,
    ProgramValueId checkpoint_input_count,
    ProgramValueId final_input_count)
{
    const auto outcome_value = Constant(b, f, block,
        TypeRef::Named(OutcomeSchema(flavor)),
        EnumValue{OutcomeSchema(flavor), static_cast<std::int64_t>(outcome)},
        "result/outcome");
    const auto optional_manifest = Need(b.AddInstruction(
        f, block, InstructionOpcode::OptionalConstruct,
        TypeRef::Named(OptionalManifestSchema(flavor)),
        manifest ? std::span<const ProgramValueId>(&*manifest, 1)
                 : std::span<const ProgramValueId>{},
        {}, "result/observed-manifest"), "optional manifest");
    const auto optional_transition = Need(b.AddInstruction(
        f, block, InstructionOpcode::OptionalConstruct,
        TypeRef::Named(OptionalTransitionSchema(flavor)),
        transition ? std::span<const ProgramValueId>(&*transition, 1)
                   : std::span<const ProgramValueId>{},
        {}, "result/observed-transition"), "optional transition");
    if (flavor == PhaseFlavor::Replay)
        return Construct(b, f, block, ResultType(flavor),
            std::array{outcome_value, mismatch_turn, expected_rng, observed_rng,
                       optional_manifest, optional_transition},
            "result/value");
    return Construct(b, f, block, ResultType(flavor),
        std::array{outcome_value, mismatch_turn, expected_rng, observed_rng,
                   optional_manifest, optional_transition, anchor,
                   checkpoint_input_count, final_input_count}, "result/value");
}

BasicBlock& Block(ProgramFunction& function, ProgramBlockId id)
{
    const auto found = std::ranges::find(function.blocks, id, &BasicBlock::id);
    if (found == function.blocks.end())
        throw std::logic_error("battle.record block identity disappeared");
    return *found;
}

ProgramModule ConstructModule(const BattleReplayPlanV1& plan, PhaseFlavor flavor)
{
    ProgramModule module{.identity = {
        .canonical_id = SpecializedModuleId(plan, flavor), .revision = 1}};
    const auto command = LowerInteraction(
        battlesingleturn::BattleCommandInteractionV3(), module);
    const auto completion = LowerBattleCompletionSequenceV1(module);
    if (!command || !command.function || !completion || !completion.function)
        throw std::logic_error("Battle record interactions could not be lowered");

    Builder b(module, flavor == PhaseFlavor::Record
        ? "BattleRecordModule" : "BattleReplayModule",
        flavor == PhaseFlavor::Record
            ? "battle.record/record/v1" : "battle.replay/replay/v1");
    std::vector<RecordFieldDefinition> request_fields;
    if (flavor == PhaseFlavor::Record)
    {
        request_fields.push_back({"recording_config", CanonicalRuntimeType(
            CanonicalRuntimeSchema::MovieRecordingStaticConfig)});
        request_fields.push_back({"save_request", CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact)});
    }
    request_fields.insert(request_fields.end(), {
        {"seed_frame", CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload)},
        {"expected_manifest", TypeRef::Named(
            capabilities::BattleCompletionManifestSchemaIdentity())},
        {"expected_transition", TypeRef::Named(
            capabilities::FieldTransitionContextSchemaIdentity())},
        {"battle_set_id", TypeRef::Builtin(BuiltinType::U64)},
        {"wave_id", TypeRef::Builtin(BuiltinType::U64)},
        {"turn_job_id", TypeRef::Builtin(BuiltinType::U64)},
        {"execution_job_id", TypeRef::Builtin(BuiltinType::U64)},
    });
    for (std::size_t index = 0; index < plan.turns.size(); ++index)
        request_fields.push_back({"turn_" + std::to_string(index + 1),
            TypeRef::Named(capabilities::BattleTurnExecutionSpecSchemaIdentity())});
    b.AddLocalType({.identity = RequestSchema(plan.turns.size(), flavor),
        .kind = TypeSchemaKind::Record, .record_fields = std::move(request_fields)});
    b.AddLocalType({.identity = OutcomeSchema(flavor), .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {{flavor == PhaseFlavor::Record ? "Recorded" : "Matched", 0},
                         {"ReplayMismatch", 1}}});
    b.AddLocalType({.identity = OptionalManifestSchema(flavor),
        .kind = TypeSchemaKind::Optional,
        .element_type = TypeRef::Named(
            capabilities::BattleCompletionManifestSchemaIdentity())});
    b.AddLocalType({.identity = OptionalTransitionSchema(flavor),
        .kind = TypeSchemaKind::Optional,
        .element_type = TypeRef::Named(
            capabilities::FieldTransitionContextSchemaIdentity())});
    std::vector<RecordFieldDefinition> result_fields{
            {"outcome", TypeRef::Named(OutcomeSchema(flavor))},
            {"mismatch_turn", TypeRef::Builtin(BuiltinType::U32)},
            {"expected_rng", TypeRef::Builtin(BuiltinType::U32)},
            {"observed_rng", TypeRef::Builtin(BuiltinType::U32)},
            {"observed_manifest", TypeRef::Named(OptionalManifestSchema(flavor))},
            {"observed_transition", TypeRef::Named(OptionalTransitionSchema(flavor))},
    };
    if (flavor == PhaseFlavor::Record)
    {
        result_fields.push_back({"anchor_input_count", TypeRef::Builtin(BuiltinType::U64)});
        result_fields.push_back({"checkpoint_input_count", TypeRef::Builtin(BuiltinType::U64)});
        result_fields.push_back({"final_input_count", TypeRef::Builtin(BuiltinType::U64)});
    }
    b.AddLocalType({.identity = ResultSchema(flavor),
        .kind = TypeSchemaKind::Record, .record_fields = std::move(result_fields)});

    b.AddCapabilityImport(capabilities::BattlePackIdentity());
    b.AddCapabilityImport(capabilities::BattleCompletionPackIdentity());
    b.AddActionImport(capabilities::BattleCaptureContextActionIdentity());
    b.AddReducerImport(CanonicalReducerIdentity(
        CanonicalReducer::BattlePrepareCommandInteraction));
    b.AddReducerImport(capabilities::BattleCompletionSemanticEqualReducerIdentity());
    for (const auto& schema : {
        capabilities::BattleCaptureContextRequestSchemaIdentity(),
        capabilities::BattleContextSchemaIdentity(),
        capabilities::BattleCommandPreparationSchemaIdentity(),
        capabilities::BattleCommandStateSchemaIdentity(),
        capabilities::BattleCommandReceiptSchemaIdentity(),
        capabilities::BattleTurnExecutionSpecSchemaIdentity(),
        capabilities::BattleCompletionManifestSchemaIdentity(),
        capabilities::FieldTransitionContextSchemaIdentity()})
        b.AddTypeImport(schema);
    for (const auto action : {
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        CanonicalAction::InputAcquireLease,
        CanonicalAction::InputBeginDelivery,
        CanonicalAction::InputCompleteDelivery,
        CanonicalAction::ExecutionRequirePausedPc,
        CanonicalAction::ExecutionContinueUntil,
        CanonicalAction::GuestReadU32}) AddAction(b, action);
    if (flavor == PhaseFlavor::Record)
    {
        AddAction(b, CanonicalAction::ExecutionContinueUntilInputObserved);
        AddAction(b, CanonicalAction::MovieStartRecording);
        AddAction(b, CanonicalAction::MovieStopRecording);
        AddAction(b, CanonicalAction::SavestateSaveImmutableArtifact);
    }
    else
    {
        AddAction(b, CanonicalAction::MovieObserveState);
        AddAction(b, CanonicalAction::MovieStopPlayback);
    }

    const auto argument = b.NewArgument(RequestType(plan.turns.size(), flavor));
    auto& f = b.AddFunction(std::string(EntrypointId(flavor)), std::array{argument},
        ResultType(flavor), TypeRef::Builtin(BuiltinType::Bool), true);
    const auto startup_id = b.AddBlock(f).id;
    const auto movie_scope = b.NewScope();
    (void)b.AddInstruction(f, Block(f, startup_id), InstructionOpcode::EnterScope,
        std::nullopt, {}, {}, "movie/scope", std::nullopt, movie_scope);
    std::optional<ProgramValueId> recording;
    ProgramBlockId body_id = startup_id;
    if (flavor == PhaseFlavor::Record)
    {
        auto& startup = Block(f, startup_id);
        const auto adopt_request = Construct(b, f, startup,
            CanonicalActionInputType(
                CanonicalAction::MovieAdoptRestoredReadOnlyPlayback),
            std::span<const ProgramValueId>{}, "movie/adopt-request");
        const auto playback = Await(
            b, f, startup,
            CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
            adopt_request, "movie/adopt-restored-playback", movie_scope);
        (void)RequirePc(b, f, startup, kBeforeSeed, "entry");
        const auto recording_config = Project(b, f, startup, argument.id,
            CanonicalRuntimeType(CanonicalRuntimeSchema::MovieRecordingStaticConfig),
            "recording_config");
        const auto recording_request = Construct(b, f, startup,
            CanonicalActionInputType(CanonicalAction::MovieStartRecording),
            std::array{playback, recording_config}, "movie/branch-request",
            movie_scope);
        recording = Await(b, f, startup, CanonicalAction::MovieStartRecording,
            recording_request, "movie/branch-to-recording", movie_scope);
    }
    else
    {
        const auto paired_id = b.AddBlock(f).id;
        const auto inactive_id = b.AddBlock(f).id;
        const auto invalid_id = b.AddBlock(f).id;
        body_id = b.AddBlock(f).id;

        auto& startup = Block(f, startup_id);
        const auto observe_request = Construct(b, f, startup,
            CanonicalActionInputType(CanonicalAction::MovieObserveState),
            std::span<const ProgramValueId>{}, "movie/observe-request");
        const auto observation = Await(b, f, startup,
            CanonicalAction::MovieObserveState, observe_request,
            "movie/observe-restored-state");
        const auto state = Project(b, f, startup, observation,
            CanonicalRuntimeType(CanonicalRuntimeSchema::MovieState),
            "state");
        b.SetTerminator(f, startup, {
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = state,
            .enum_cases = {
                {static_cast<std::int64_t>(MovieState::Inactive),
                    {.target = inactive_id}},
                {static_cast<std::int64_t>(MovieState::ReadOnlyPlayback),
                    {.target = paired_id}},
            },
            .default_edge = BlockEdge{.target = invalid_id},
        }, "movie/dispatch-restored-state");

        auto& inactive = Block(f, inactive_id);
        (void)RequirePc(b, f, inactive, kBeforeSeed, "entry/inactive");
        b.SetTerminator(f, inactive, {
            .kind = TerminatorKind::Branch,
            .edges = {{.target = body_id}},
        }, "movie/inactive-ready");

        auto& paired = Block(f, paired_id);
        const auto adopt_request = Construct(b, f, paired,
            CanonicalActionInputType(
                CanonicalAction::MovieAdoptRestoredReadOnlyPlayback),
            std::span<const ProgramValueId>{}, "movie/adopt-request");
        const auto playback = Await(b, f, paired,
            CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
            adopt_request, "movie/adopt-restored-playback", movie_scope);
        (void)RequirePc(b, f, paired, kBeforeSeed, "entry/paired");
        (void)Await(b, f, paired, CanonicalAction::MovieStopPlayback,
            playback, "movie/stop-playback");
        const auto verify_request = Construct(b, f, paired,
            CanonicalActionInputType(CanonicalAction::MovieObserveState),
            std::span<const ProgramValueId>{}, "movie/verify-inactive-request");
        const auto verified = Await(b, f, paired,
            CanonicalAction::MovieObserveState, verify_request,
            "movie/verify-inactive");
        const auto verified_state = Project(b, f, paired, verified,
            CanonicalRuntimeType(CanonicalRuntimeSchema::MovieState),
            "state");
        b.SetTerminator(f, paired, {
            .kind = TerminatorKind::EnumSwitch,
            .condition_or_selector = verified_state,
            .enum_cases = {{
                static_cast<std::int64_t>(MovieState::Inactive),
                {.target = body_id}}},
            .default_edge = BlockEdge{.target = invalid_id},
        }, "movie/require-inactive");

        auto& invalid = Block(f, invalid_id);
        (void)b.AddInstruction(f, invalid, InstructionOpcode::ExitScope,
            std::nullopt, {}, {}, "movie/invalid-state-scope",
            std::nullopt, movie_scope);
        b.SetTerminator(f, invalid, {
            .kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{
                "battle_replay_movie_state_invalid",
                "Battle Replay requires restored movie state Inactive or ReadOnlyPlayback and must enter replay with MovieState::Inactive"},
        }, "movie/invalid-state");
    }
    auto& entry = Block(f, body_id);
    const auto seed_frame = Project(b, f, entry, argument.id,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        "seed_frame");
    const std::array seed_endpoint{
        SemanticPointReference{.capability_pack = capabilities::FieldPackIdentity(),
            .canonical_id = "soa.field.point.prebattle.AfterRandSeedSet",
            .kind = SemanticPointKind::ProgramCounter, .physical_pc = kAfterSeed}};
    const auto seed = LowerSynchronizedFrameDelivery(
        b, f, entry, seed_frame, seed_endpoint, "seed");
    if (!seed) throw std::logic_error("Battle record seed delivery failed to lower");
    const std::array turn_input_point{BattlePoint("TurnInputs", kTurnInputs)};
    ProgramValueId turn_stop = ContinueTo(b, f, entry, turn_input_point,
        "turn/1/input");

    const auto expected_manifest = Project(b, f, entry, argument.id,
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        "expected_manifest");
    const auto expected_transition = Project(b, f, entry, argument.id,
        TypeRef::Named(capabilities::FieldTransitionContextSchemaIdentity()),
        "expected_transition");
    const auto zero_u32 = Constant(b, f, entry,
        TypeRef::Builtin(BuiltinType::U32), std::uint32_t{0}, "zero-u32");
    const auto zero_u64 = Constant(b, f, entry,
        TypeRef::Builtin(BuiltinType::U64), std::uint64_t{0}, "zero-u64");

    ProgramBlockId current_id = entry.id;
    ProgramValueId last_anchor = zero_u64;
    ProgramValueId victory_stop{};
    for (std::size_t index = 0; index < plan.turns.size(); ++index)
    {
        const auto& expected = plan.turns[index];
        auto& current = Block(f, current_id);
        const auto context = CaptureContext(b, f, current, turn_stop,
            std::format("turn/{}/context", expected.turn_index));
        const auto plan_value = Project(b, f, current, argument.id,
            TypeRef::Named(capabilities::BattleTurnExecutionSpecSchemaIdentity()),
            "turn_" + std::to_string(index + 1));
        const auto prepared_plan = Need(b.AddInstruction(f, current,
            InstructionOpcode::CallReducer,
            TypeRef::Named(capabilities::BattleCommandPreparationSchemaIdentity()),
            std::array{context, plan_value},
            Reducer(CanonicalReducer::BattlePrepareCommandInteraction),
            std::format("turn/{}/validate", expected.turn_index)),
            "Battle Plan preparation");
        const auto valid = Project(b, f, current, prepared_plan,
            TypeRef::Builtin(BuiltinType::Bool), "success");
        const auto state = Project(b, f, current, prepared_plan,
            TypeRef::Named(capabilities::BattleCommandStateSchemaIdentity()), "state");
        const auto execute_id = b.AddBlock(f).id;
        const auto invalid_id = b.AddBlock(f).id;
        b.SetTerminator(f, Block(f, current_id), {.kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = valid,
            .edges = {{.target = execute_id}, {.target = invalid_id}}},
            std::format("turn/{}/plan-dispatch", expected.turn_index));
        (void)b.AddInstruction(f, Block(f, invalid_id),
            InstructionOpcode::ExitScope, std::nullopt, {}, {},
            std::format("turn/{}/invalid-plan-scope", expected.turn_index),
            std::nullopt, movie_scope);
        b.SetTerminator(f, Block(f, invalid_id), {.kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{"battle_replay_plan_invalid",
                "Resolved Battle replay command is inconsistent with live state"}},
            std::format("turn/{}/invalid-plan", expected.turn_index));

        GCInputFrame a_frame{}; a_frame.A();
        GCInputFrame b_frame{}; b_frame.B();
        GCInputFrame up_frame{}; up_frame.DUp();
        GCInputFrame down_frame{}; down_frame.DDown();
        const auto input_type = CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload);
        auto& execute = Block(f, execute_id);
        const auto press_a = Constant(b, f, execute, input_type,
            FrameBytes(a_frame), "input/a");
        const auto press_b = Constant(b, f, execute, input_type,
            FrameBytes(b_frame), "input/b");
        const auto press_up = Constant(b, f, execute, input_type,
            FrameBytes(up_frame), "input/up");
        const auto press_down = Constant(b, f, execute, input_type,
            FrameBytes(down_frame), "input/down");
        const auto command_receipt = Need(b.AddInstruction(f, execute,
            InstructionOpcode::CallLocal,
            TypeRef::Named(capabilities::BattleCommandReceiptSchemaIdentity()),
            std::array{state, press_a, press_b, press_up, press_down},
            {.kind = InstructionTargetKind::LocalFunction,
             .local_function = *command.function},
            std::format("turn/{}/command", expected.turn_index)),
            "Battle command interaction");
        last_anchor = Project(b, f, execute, command_receipt,
            TypeRef::Builtin(BuiltinType::U64),
            "last_command_commit_input_count");
        const std::array terminal_points{
            BattlePoint("TurnInputs", kTurnInputs),
            BattlePoint("EndBattleVictory", kVictory),
            BattlePoint("Battle_Defeat", kDefeat)};
        const auto terminal = ContinueTo(b, f, execute, terminal_points,
            std::format("turn/{}/terminal", expected.turn_index));
        const auto observed_pc = Project(b, f, execute, terminal,
            TypeRef::Builtin(BuiltinType::U32), "pc");
        const auto observed_rng = ReadScalar(b, f, execute, terminal,
            CanonicalAction::GuestReadU32, kRng,
            std::format("turn/{}/rng", expected.turn_index));
        const auto expected_pc_value = Constant(b, f, execute,
            TypeRef::Builtin(BuiltinType::U32),
            expected.expected_outcome ==
                battlesingleturn::BattleSingleTurnOutcomeV1::Victory
                    ? kVictory : kTurnInputs,
            std::format("turn/{}/expected-pc", expected.turn_index));
        const auto expected_rng_value = Constant(b, f, execute,
            TypeRef::Builtin(BuiltinType::U32), expected.expected_ending_rng,
            std::format("turn/{}/expected-rng", expected.turn_index));
        const auto pc_ok = Equal(b, f, execute, observed_pc, expected_pc_value,
            std::format("turn/{}/pc-ok", expected.turn_index));
        const auto rng_ok = Equal(b, f, execute, observed_rng, expected_rng_value,
            std::format("turn/{}/rng-ok", expected.turn_index));
        const auto matches = BoolAnd(b, f, execute, pc_ok, rng_ok,
            std::format("turn/{}/matches", expected.turn_index));
        const auto matched_id = b.AddBlock(f).id;
        const auto mismatch_id = b.AddBlock(f).id;
        b.SetTerminator(f, Block(f, execute_id), {.kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = matches,
            .edges = {{.target = matched_id}, {.target = mismatch_id}}},
            std::format("turn/{}/comparison", expected.turn_index));
        auto& mismatch = Block(f, mismatch_id);
        const auto turn_index = Constant(b, f, mismatch,
            TypeRef::Builtin(BuiltinType::U32), expected.turn_index,
            "mismatch/turn");
        const auto mismatch_result = Result(b, f, mismatch, flavor,
            battlecompletion::BattleRecordOutcomeV1::ReplayMismatch,
            turn_index, expected_rng_value, observed_rng,
            std::nullopt, std::nullopt, last_anchor, zero_u64, zero_u64);
        const auto domain_success = Constant(b, f, mismatch,
            TypeRef::Builtin(BuiltinType::Bool), true, "mismatch/domain-success");
        (void)b.AddInstruction(f, mismatch, InstructionOpcode::ExitScope,
            std::nullopt, {}, {},
            std::format("turn/{}/mismatch-scope", expected.turn_index),
            std::nullopt, movie_scope);
        b.SetTerminator(f, mismatch, {.kind = TerminatorKind::Return,
            .return_value = mismatch_result, .domain_outcome = domain_success},
            std::format("turn/{}/mismatch", expected.turn_index));
        current_id = matched_id;
        turn_stop = terminal;
        if (index + 1 == plan.turns.size()) victory_stop = terminal;
    }

    auto& completion_block = Block(f, current_id);
    const auto victory_pc = Project(b, f, completion_block, victory_stop,
        TypeRef::Builtin(BuiltinType::U32), "pc");
    const auto victory_vi = Project(b, f, completion_block, victory_stop,
        TypeRef::Builtin(BuiltinType::U64), "vi_count");
    const auto victory_epoch = Project(b, f, completion_block, victory_stop,
        TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
    const auto neutral = Constant(b, f, completion_block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        std::vector<Byte>{0,0,128,128,128,128,0,0}, "completion/neutral");
    const auto battle_set_id = Project(b, f, completion_block, argument.id,
        TypeRef::Builtin(BuiltinType::U64), "battle_set_id");
    const auto wave_id = Project(b, f, completion_block, argument.id,
        TypeRef::Builtin(BuiltinType::U64), "wave_id");
    const auto turn_job_id = Project(b, f, completion_block, argument.id,
        TypeRef::Builtin(BuiltinType::U64), "turn_job_id");
    const auto execution_job_id = Project(b, f, completion_block, argument.id,
        TypeRef::Builtin(BuiltinType::U64), "execution_job_id");
    const auto sequence = Need(b.AddInstruction(f, completion_block,
        InstructionOpcode::CallLocal,
        TypeRef::Named(BattleCompletionSequenceReceiptSchemaIdentity()),
        std::array{victory_pc, victory_vi, victory_epoch, neutral,
                   battle_set_id, wave_id, turn_job_id, execution_job_id},
        {.kind = InstructionTargetKind::LocalFunction,
         .local_function = *completion.function},
        "completion/sequence"), "Battle completion sequence");
    const auto manifest = Project(b, f, completion_block, sequence,
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        "manifest");
    const auto transition = Project(b, f, completion_block, sequence,
        TypeRef::Named(capabilities::FieldTransitionContextSchemaIdentity()),
        "transition");
    const auto terminal_input_count = Project(b, f, completion_block, sequence,
        TypeRef::Builtin(BuiltinType::U64), "terminal_movie_input_count");
    const auto semantic_equal = Need(b.AddInstruction(f, completion_block,
        InstructionOpcode::CallReducer, TypeRef::Builtin(BuiltinType::Bool),
        std::array{manifest, expected_manifest},
        Reducer(capabilities::BattleCompletionSemanticEqualReducerIdentity()),
        "completion/semantic-equality"), "manifest comparison");
    const auto publish_id = b.AddBlock(f).id;
    const auto completion_mismatch_id = b.AddBlock(f).id;
    b.SetTerminator(f, Block(f, current_id), {.kind = TerminatorKind::ConditionalBranch,
        .condition_or_selector = semantic_equal,
        .edges = {{.target = publish_id}, {.target = completion_mismatch_id}}},
        "completion/comparison");
    auto& completion_mismatch = Block(f, completion_mismatch_id);
    const auto final_turn = Constant(b, f, completion_mismatch,
        TypeRef::Builtin(BuiltinType::U32), plan.turns.back().turn_index,
        "completion-mismatch/turn");
    const auto completion_mismatch_result = Result(b, f, completion_mismatch, flavor,
        battlecompletion::BattleRecordOutcomeV1::ReplayMismatch,
        final_turn, zero_u32, zero_u32,
        std::optional<ProgramValueId>{manifest},
        std::optional<ProgramValueId>{transition}, last_anchor,
        zero_u64, zero_u64);
    const auto mismatch_success = Constant(b, f, completion_mismatch,
        TypeRef::Builtin(BuiltinType::Bool), true, "completion-mismatch/success");
    (void)b.AddInstruction(f, completion_mismatch,
        InstructionOpcode::ExitScope, std::nullopt, {}, {},
        "completion-mismatch/scope", std::nullopt, movie_scope);
    b.SetTerminator(f, completion_mismatch, {.kind = TerminatorKind::Return,
        .return_value = completion_mismatch_result,
        .domain_outcome = mismatch_success}, "completion-mismatch/return");

    auto& publish = Block(f, publish_id);
    ProgramValueId final_input_count = terminal_input_count;
    if (flavor == PhaseFlavor::Record)
    {
        const auto save_request = Project(b, f, publish, argument.id,
            CanonicalActionInputType(CanonicalAction::SavestateSaveImmutableArtifact),
            "save_request");
        (void)Await(b, f, publish, CanonicalAction::SavestateSaveImmutableArtifact,
            save_request, "record/capture-checkpoint-sav");

        const auto tail_scope = b.NewScope();
        (void)b.AddInstruction(f, publish, InstructionOpcode::EnterScope,
            std::nullopt, {}, {}, "record/neutral-tail/scope", std::nullopt,
            tail_scope);
        const auto lease_config = Constant(b, f, publish,
            CanonicalRuntimeType(CanonicalRuntimeSchema::InputLeaseStaticConfig),
            LeaseConfig(), "record/neutral-tail/lease-config");
        const auto lease_request = Construct(b, f, publish,
            CanonicalActionInputType(CanonicalAction::InputAcquireLease),
            std::array{lease_config}, "record/neutral-tail/lease-request",
            tail_scope);
        const auto lease = Await(b, f, publish,
            CanonicalAction::InputAcquireLease, lease_request,
            "record/neutral-tail/lease", tail_scope);
        const auto begin_request = Construct(b, f, publish,
            CanonicalActionInputType(CanonicalAction::InputBeginDelivery),
            std::array{lease, neutral}, "record/neutral-tail/begin-request",
            tail_scope);
        const auto binding = Await(b, f, publish,
            CanonicalAction::InputBeginDelivery, begin_request,
            "record/neutral-tail/binding");
        const auto advance_config = Constant(b, f, publish,
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig),
            AdvanceConfig(), "record/neutral-tail/execution-config");
        const auto observe_request = Construct(b, f, publish,
            CanonicalActionInputType(
                CanonicalAction::ExecutionContinueUntilInputObserved),
            std::array{binding, terminal_input_count, advance_config},
            "record/neutral-tail/observe-request", tail_scope);
        const auto observed = Await(b, f, publish,
            CanonicalAction::ExecutionContinueUntilInputObserved,
            observe_request, "record/neutral-tail/observe-poll");
        final_input_count = Project(b, f, publish, observed,
            TypeRef::Builtin(BuiltinType::U64), "movie_input_count");
        const auto complete_request = Construct(b, f, publish,
            CanonicalActionInputType(CanonicalAction::InputCompleteDelivery),
            std::array{lease, binding}, "record/neutral-tail/complete-request",
            tail_scope);
        (void)Await(b, f, publish, CanonicalAction::InputCompleteDelivery,
            complete_request, "record/neutral-tail/complete");
        (void)b.AddInstruction(f, publish, InstructionOpcode::ExitScope,
            std::nullopt, {}, {}, "record/neutral-tail/release-scope",
            std::nullopt, tail_scope);

        const auto finalized_dtm = Await(b, f, publish,
            CanonicalAction::MovieStopRecording, *recording,
            "record/finalize-extended-dtm");
        (void)b.AddInstruction(f, publish,
            InstructionOpcode::PublishArtifact, std::nullopt,
            std::array{finalized_dtm}, {}, "record/publish-extended-dtm");
    }
    (void)b.AddInstruction(f, publish, InstructionOpcode::ExitScope,
        std::nullopt, {}, {}, "movie/scope-complete", std::nullopt,
        movie_scope);
    const auto recorded_result = Result(b, f, publish, flavor,
        battlecompletion::BattleRecordOutcomeV1::Recorded,
        zero_u32, zero_u32, zero_u32,
        std::optional<ProgramValueId>{manifest},
        std::optional<ProgramValueId>{transition}, last_anchor,
        terminal_input_count, final_input_count);
    const auto recorded = Constant(b, f, publish,
        TypeRef::Builtin(BuiltinType::Bool), true, "record/domain-success");
    b.SetTerminator(f, publish, {.kind = TerminatorKind::Return,
        .return_value = recorded_result, .domain_outcome = recorded},
        "record/return");

    module.accepted_policies = {
        .state_policies = {InvocationStatePolicy::RestoreBaseline},
        .execution_intents = {ExecutionIntent::Live},
        .permits_movie_playback = true,
        .permits_movie_recording = flavor == PhaseFlavor::Record};
    module.budgets = {.maximum_instructions = 2'000'000,
        .maximum_calls = 16'384, .maximum_call_depth = 8,
        .maximum_action_requests = 262'144, .maximum_emissions = 1024,
        // Program budgets are required to be finite and nonzero on every
        // axis. Replay still declares no artifact schemas, so the executor
        // cannot admit an artifact-bearing result despite this capacity.
        .maximum_artifacts = flavor == PhaseFlavor::Record ? 2u : 1u,
        .maximum_values = 524'288,
        .maximum_value_bytes = 64 * 1024 * 1024,
        .maximum_trace_events = 2'000'000};
    std::vector<SchemaIdentity> artifact_schemas;
    if (flavor == PhaseFlavor::Record)
    {
        const auto savestate_artifact = CanonicalActionArtifactPayloadSchemaIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact);
        const auto movie_artifact = CanonicalActionArtifactPayloadSchemaIdentity(
            CanonicalAction::MovieStopRecording);
        if (!savestate_artifact || !movie_artifact)
            throw std::logic_error("Battle record artifact schemas are unavailable");
        b.AddTypeImport(*savestate_artifact);
        b.AddTypeImport(*movie_artifact);
        artifact_schemas = {*savestate_artifact, *movie_artifact};
    }
    module.entrypoints = {{.name = std::string(EntrypointId(flavor)), .function = f.id,
        .input_type = RequestType(plan.turns.size(), flavor),
        .output_type = ResultType(flavor),
        .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
        .artifact_schemas = std::move(artifact_schemas),
        .required_capability_packs = module.required_capability_packs,
        .accepted_policies = module.accepted_policies}};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

RuntimeProfile Profile(const ProgramDependencyLock& dependencies)
{
    return {.profile_id = "soa-usa-jit64-v1",
        .game_id = std::string(capabilities::kSupportedGameId),
        .disc_identity = std::string(capabilities::kSupportedGameId),
        .executable_identity = std::string(capabilities::kSupportedExecutableIdentity),
        .backend = "jit64", .capability_packs = dependencies.capability_packs};
}

std::string ProfileHash(const RuntimeProfile& profile)
{
    std::string bytes = profile.profile_id + '\0' + profile.game_id + '\0' +
        profile.disc_identity + '\0' + profile.executable_identity + '\0' +
        profile.backend;
    return hash::sha256(bytes.data(), bytes.size());
}

std::optional<ProgramDependencyLock> Verify(
    const ProgramModule& module, std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const auto registered = capabilities::RegisterSourceCapabilityPacks(
        schemas, actions, packs);
    if (!registered.success) { Diagnostic(diagnostic, registered.error.message); return {}; }
    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success) { Diagnostic(diagnostic, stored.error.message); return {}; }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const auto verified = verifier.Verify(stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        std::string message = "battle.record verification failed";
        for (const auto& item : verified.diagnostics)
        {
            message += "; " + item.message;
            if (item.source_location)
            {
                const auto source = std::ranges::find(
                    module.source_map.entries, *item.source_location,
                    &SourceMapEntry::id);
                if (source != module.source_map.entries.end())
                    message += " at " + source->semantic_path;
            }
        }
        Diagnostic(diagnostic, std::move(message)); return {};
    }
    return verified.verified->dependency_lock;
}

class GraphAssembler
{
public:
    ProgramValueId Add(TypeRef type, ProgramValuePayload payload)
    {
        const auto id = ProgramValueId(next_++);
        values_.push_back({id, std::move(type), std::move(payload)}); return id;
    }
    ProgramValueId Import(const ProgramValueGraph& graph)
    {
        std::map<ProgramValueId, ProgramValueId> ids;
        for (const auto& value : graph.values)
            ids.emplace(value.id, ProgramValueId(next_++));
        for (const auto& value : graph.values)
        {
            auto copy = value; copy.id = ids.at(value.id);
            if (auto* record = std::get_if<RecordValue>(&copy.payload))
                for (auto& field : record->fields) field = ids.at(field);
            if (auto* list = std::get_if<ListValue>(&copy.payload))
                for (auto& element : list->elements) element = ids.at(element);
            if (auto* optional = std::get_if<OptionalValue>(&copy.payload);
                optional && optional->value) optional->value = ids.at(*optional->value);
            values_.push_back(std::move(copy));
        }
        return ids.at(graph.root);
    }
    ProgramValueGraph Finish(ProgramValueId root)
    { return {root, std::move(values_)}; }
private:
    std::uint64_t next_ = 1;
    std::vector<ProgramValue> values_;
};

ProgramValueGraph InputGraph(
    const BattleReplayPlanV1& plan,
    const BattleRecordRequestV1& request,
    std::string* diagnostic)
{
    GraphAssembler graph;
    const auto encode_action = [&](CanonicalAction action,
                                   CanonicalActionPayload payload)
        -> std::optional<ProgramValueId>
    {
        const auto encoded = EncodeCanonicalActionPayload(payload,
            *CanonicalActionInputType(action).named);
        if (!encoded.ok) { Diagnostic(diagnostic, encoded.diagnostic); return {}; }
        return graph.Import(encoded.graph);
    };
    CanonicalActionPayload save_payload;
    save_payload.AddUtf8(CanonicalActionPayloadField::Path,
        request.output_preseed_savestate_path);
    save_payload.AddUtf8(CanonicalActionPayloadField::Label,
        "battle.record paired accepted preseed");
    save_payload.AddUnsigned(
        CanonicalActionPayloadField::MovieArtifactMode,
        static_cast<std::uint64_t>(
            SavestateMovieArtifactMode::DeferredFinalRecordingPair));
    const auto save = encode_action(
        CanonicalAction::SavestateSaveImmutableArtifact, std::move(save_payload));
    if (!save) return {};
    const auto recording = graph.Add(
        CanonicalRuntimeType(CanonicalRuntimeSchema::MovieRecordingStaticConfig),
        RecordingConfig(request.output_dtm_path, "battle.record selected lineage"));
    const auto seed = graph.Add(
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        FrameBytes(plan.confirmed_seed_frame));
    std::vector<std::uint8_t> manifest_bytes;
    std::vector<std::uint8_t> transition_bytes;
    if (!battlecompletion::EncodeBattleCompletionManifestV1(
            plan.expected_completion, manifest_bytes) ||
        !battlecompletion::EncodeFieldTransitionContextV1(
            plan.expected_completion.transition, transition_bytes)) return {};
    const auto manifest = graph.Add(
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        std::move(manifest_bytes));
    const auto transition = graph.Add(
        TypeRef::Named(capabilities::FieldTransitionContextSchemaIdentity()),
        std::move(transition_bytes));
    std::vector<ProgramValueId> fields{recording, *save, seed,
        manifest, transition,
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.battle_set_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.wave_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.turn_job_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.execution_job_id)};
    for (const auto& turn : plan.turns)
        fields.push_back(graph.Import(
            capabilities::EncodeBattleTurnExecutionSpecValue(turn.plan)));
    const auto root = graph.Add(RequestType(plan.turns.size(), PhaseFlavor::Record),
        RecordValue{std::move(fields)});
    return graph.Finish(root);
}

ProgramValueGraph ReplayInputGraph(
    const BattleReplayPlanV1& plan, std::string* diagnostic)
{
    GraphAssembler graph;
    const auto seed = graph.Add(
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        FrameBytes(plan.confirmed_seed_frame));
    std::vector<std::uint8_t> manifest_bytes;
    std::vector<std::uint8_t> transition_bytes;
    if (!battlecompletion::EncodeBattleCompletionManifestV1(
            plan.expected_completion, manifest_bytes) ||
        !battlecompletion::EncodeFieldTransitionContextV1(
            plan.expected_completion.transition, transition_bytes))
    {
        Diagnostic(diagnostic, "battle.replay expected completion could not be encoded");
        return {};
    }
    const auto manifest = graph.Add(
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        std::move(manifest_bytes));
    const auto transition = graph.Add(
        TypeRef::Named(capabilities::FieldTransitionContextSchemaIdentity()),
        std::move(transition_bytes));
    std::vector<ProgramValueId> fields{seed, manifest, transition,
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.battle_set_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.wave_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.turn_job_id),
        graph.Add(TypeRef::Builtin(BuiltinType::U64),
            plan.selected_lineage.execution_job_id)};
    for (const auto& turn : plan.turns)
        fields.push_back(graph.Import(
            capabilities::EncodeBattleTurnExecutionSpecValue(turn.plan)));
    const auto root = graph.Add(
        RequestType(plan.turns.size(), PhaseFlavor::Replay),
        RecordValue{std::move(fields)});
    return graph.Finish(root);
}

const ProgramValue* Find(const ProgramValueGraph& graph, ProgramValueId id)
{
    const auto found = std::ranges::find(graph.values, id, &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

bool Scalar(const ProgramValueGraph& graph, ProgramValueId id,
            std::uint32_t& output)
{
    const auto* value = Find(graph, id);
    const auto* scalar = value ? std::get_if<std::uint32_t>(&value->payload) : nullptr;
    if (!scalar) return false; output = *scalar; return true;
}
bool Scalar(const ProgramValueGraph& graph, ProgramValueId id,
            std::uint64_t& output)
{
    const auto* value = Find(graph, id);
    const auto* scalar = value ? std::get_if<std::uint64_t>(&value->payload) : nullptr;
    if (!scalar) return false; output = *scalar; return true;
}

bool DecodeOutput(
    const BattleReplayPlanV1& plan,
    const ProgramValueGraph& graph,
    BattleRecordResultV1& output,
    std::string* diagnostic)
{
    output = {};
    const auto* root = Find(graph, graph.root);
    const auto* record = root && root->type == ResultType(PhaseFlavor::Record)
        ? std::get_if<RecordValue>(&root->payload) : nullptr;
    if (!record || record->fields.size() != 9) return false;
    const auto* outcome_value = Find(graph, record->fields[0]);
    const auto* outcome = outcome_value
        ? std::get_if<EnumValue>(&outcome_value->payload) : nullptr;
    if (!outcome || outcome->schema != OutcomeSchema(PhaseFlavor::Record) ||
        (outcome->value != 0 && outcome->value != 1)) return false;
    output.outcome = static_cast<battlecompletion::BattleRecordOutcomeV1>(
        outcome->value);
    if (!Scalar(graph, record->fields[1], output.mismatch_turn) ||
        !Scalar(graph, record->fields[2], output.expected_rng) ||
        !Scalar(graph, record->fields[3], output.observed_rng)) return false;
    const auto decode_optional_bytes = [&]<typename T>(
        ProgramValueId id,
        const SchemaIdentity& optional_schema,
        const SchemaIdentity& payload_schema,
        std::optional<T>& destination,
        auto decode) -> bool
    {
        const auto* optional_value = Find(graph, id);
        const auto* optional = optional_value
            ? std::get_if<OptionalValue>(&optional_value->payload) : nullptr;
        if (!optional_value ||
            optional_value->type != TypeRef::Named(optional_schema) ||
            !optional)
            return false;
        if (!optional->value)
        {
            destination.reset();
            return true;
        }
        const auto* payload_value = Find(graph, *optional->value);
        const auto* bytes = payload_value
            ? std::get_if<std::vector<Byte>>(&payload_value->payload) : nullptr;
        T value{};
        if (!payload_value ||
            payload_value->type != TypeRef::Named(payload_schema) ||
            !bytes || !decode(*bytes, value))
            return false;
        destination = std::move(value);
        return true;
    };
    std::uint64_t anchor = 0;
    if (!decode_optional_bytes(
            record->fields[4], OptionalManifestSchema(PhaseFlavor::Record),
            capabilities::BattleCompletionManifestSchemaIdentity(),
            output.observed_completion,
            [](std::span<const Byte> bytes,
               battlecompletion::BattleCompletionManifestV1& value) {
                return battlecompletion::DecodeBattleCompletionManifestV1(
                    bytes, value);
            }) ||
        !decode_optional_bytes(
            record->fields[5], OptionalTransitionSchema(PhaseFlavor::Record),
            capabilities::FieldTransitionContextSchemaIdentity(),
            output.transition,
            [](std::span<const Byte> bytes,
               battlecompletion::FieldTransitionContextV1& value) {
                return battlecompletion::DecodeFieldTransitionContextV1(
                    bytes, value);
            }) ||
        !Scalar(graph, record->fields[6], anchor) ||
        !Scalar(graph, record->fields[7], output.checkpoint_input_count) ||
        !Scalar(graph, record->fields[8], output.final_input_count))
        return false;
    output.timing_anchor = {
        .turn_index = plan.turns.back().turn_index,
        .actor_slot = plan.turns.back().plan.commands.back().actor_slot,
        .command_ordinal = static_cast<std::uint32_t>(
            plan.turns.back().plan.commands.size()),
        .semantic_role = "final_player_command_commitment",
        .dtm_input_index = anchor};
    Diagnostic(diagnostic, {}); return true;
}

bool DecodeReplayOutput(
    const ProgramValueGraph& graph,
    battlereplay::BattleReplayResultV1& output,
    std::string* diagnostic)
{
    output = {};
    const auto* root = Find(graph, graph.root);
    const auto* record = root && root->type == ResultType(PhaseFlavor::Replay)
        ? std::get_if<RecordValue>(&root->payload) : nullptr;
    if (!record || record->fields.size() != 6) return false;
    const auto* outcome_value = Find(graph, record->fields[0]);
    const auto* outcome = outcome_value
        ? std::get_if<EnumValue>(&outcome_value->payload) : nullptr;
    if (!outcome || outcome->schema != OutcomeSchema(PhaseFlavor::Replay) ||
        (outcome->value != 0 && outcome->value != 1)) return false;
    output.outcome = static_cast<battlereplay::BattleReplayOutcomeV1>(
        outcome->value);
    if (!Scalar(graph, record->fields[1], output.mismatch_turn) ||
        !Scalar(graph, record->fields[2], output.expected_rng) ||
        !Scalar(graph, record->fields[3], output.observed_rng)) return false;
    const auto decode_optional = [&]<typename T>(
        ProgramValueId id, const SchemaIdentity& optional_schema,
        const SchemaIdentity& payload_schema, std::optional<T>& destination,
        auto decode) -> bool
    {
        const auto* optional_value = Find(graph, id);
        const auto* optional = optional_value
            ? std::get_if<OptionalValue>(&optional_value->payload) : nullptr;
        if (!optional_value ||
            optional_value->type != TypeRef::Named(optional_schema) || !optional)
            return false;
        if (!optional->value) { destination.reset(); return true; }
        const auto* payload_value = Find(graph, *optional->value);
        const auto* bytes = payload_value
            ? std::get_if<std::vector<Byte>>(&payload_value->payload) : nullptr;
        T value{};
        if (!payload_value || payload_value->type != TypeRef::Named(payload_schema) ||
            !bytes || !decode(*bytes, value)) return false;
        destination = std::move(value);
        return true;
    };
    if (!decode_optional(
            record->fields[4], OptionalManifestSchema(PhaseFlavor::Replay),
            capabilities::BattleCompletionManifestSchemaIdentity(),
            output.observed_completion,
            [](std::span<const Byte> bytes,
               battlecompletion::BattleCompletionManifestV1& value) {
                return battlecompletion::DecodeBattleCompletionManifestV1(bytes, value);
            }) ||
        !decode_optional(
            record->fields[5], OptionalTransitionSchema(PhaseFlavor::Replay),
            capabilities::FieldTransitionContextSchemaIdentity(),
            output.transition,
            [](std::span<const Byte> bytes,
               battlecompletion::FieldTransitionContextV1& value) {
                return battlecompletion::DecodeFieldTransitionContextV1(bytes, value);
            }))
        return false;
    Diagnostic(diagnostic, {});
    return true;
}

bool DomainOutcomeIsTrue(const ProgramValueGraph& graph)
{
    if (!graph.root ||
        std::ranges::count(graph.values, graph.root, &ProgramValue::id) != 1)
        return false;
    const auto* root = Find(graph, graph.root);
    const auto* succeeded = root
        ? std::get_if<bool>(&root->payload) : nullptr;
    return root && root->type == TypeRef::Builtin(BuiltinType::Bool) &&
        succeeded && *succeeded;
}

class Definition final : public IBattleRecordFullPhaseDefinitionV1
{
public:
    explicit Definition(BattleReplayPlanV1 plan) : plan_(std::move(plan))
    {
        plan_.canonical_sha256 = ComputeBattleReplayPlanHashV1(plan_);
        module_ = ConstructModule(plan_, PhaseFlavor::Record);
        std::string diagnostic;
        const auto dependencies = Verify(module_, &diagnostic);
        const auto encoded = EncodeProgramModuleV1(module_);
        if (!dependencies || !encoded)
            throw std::logic_error(diagnostic.empty() ? encoded.status.message : diagnostic);
        dependencies_ = *dependencies;
        profile_ = Profile(dependencies_);
        envelope_ = {{module_.identity.canonical_id, module_.identity.revision,
            module_.identity.module_hash.ToHex()}, kProgramCodecVersionV1,
            false, encoded.bytes};
        const BattleRecordRequestV1 sample{
            .output_dtm_path = "recorded.dtm",
            .output_preseed_savestate_path = "recorded-preseed.sav"};
        runtime_ = {.module = envelope_.identity,
            .entrypoint = std::string(Entrypoint),
            .dependency_lock_sha256 =
                ComputeProgramDependencyLockHashV1(dependencies_).ToHex(),
            .runtime_profile_sha256 = ProfileHash(profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = {.intent = ExecutionIntent::Live,
                .allow_movie_playback = true,
                .allow_movie_recording = true,
                .allow_input = true,
                .record_trace = false},
            .limits = module_.budgets,
            .baseline_lineage = std::string(BaselineLineage)};
        const auto invocation = Resolve(sample, ProgramExecutionId(1), AttemptId(1),
            &diagnostic);
        if (!invocation) throw std::logic_error("battle.record sample is invalid");
        runtime_.verified_dependency_sha256 =
            ComputeProgramInvocationCompatibilityHashV1(*invocation);
        constexpr std::string_view movie =
            "soa.battle.record/playback-branch-record-finalize/v1";
        constexpr std::string_view service =
            "soa.battle.record/adaptive-battle-completion/v1";
        runtime_.movie_policy_sha256 = hash::sha256(movie.data(), movie.size());
        runtime_.service_policy_sha256 = hash::sha256(service.data(), service.size());
        const std::string canonical = std::string(FullPhaseCanonicalId) + '\0' +
            runtime_.module.canonical_hash + '\0' + plan_.canonical_sha256 + '\0' +
            runtime_.verified_dependency_sha256;
        identity_ = {static_cast<std::int32_t>(savor::PK_BattleRecord),
            ProgramVersion, std::string(FullPhaseCanonicalId), 1,
            hash::sha256(canonical.data(), canonical.size())};
    }

    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override
    { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override
    { return runtime_; }
    const EncodedModuleEnvelope& module_envelope() const noexcept override
    { return envelope_; }
    fullphase::FullPhaseWorksetPolicy workset_policy() const noexcept override
    { return {.minimum_item_count = 1, .maximum_item_count = 1}; }
    const BattleReplayPlanV1& replay_plan() const noexcept override { return plan_; }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> bytes, ProgramExecutionId execution,
        AttemptId attempt, std::string* diagnostic) const override
    {
        BattleRecordRequestV1 request;
        if (!DecodeBattleRecordExecutionInputV1(bytes, request, diagnostic)) return {};
        return Resolve(request, execution, attempt, diagnostic);
    }

    bool DecodeProgramResult(
        std::span<const Byte> bytes, BattleRecordResultV1& output,
        std::string* diagnostic) const override
    {
        const auto decoded = DecodeProgramResultV1(bytes);
        if (!decoded || !decoded.value || decoded.value->module != module_.identity ||
            decoded.value->entrypoint != Entrypoint ||
            decoded.value->resolved_dependencies != dependencies_ ||
            decoded.value->infrastructure != ProgramInfrastructureStatus::Completed ||
            decoded.value->cleanup != ProgramCleanupStatus::Clean ||
            decoded.value->session_disposition != SessionDisposition::Clean ||
            !decoded.value->output || !decoded.value->domain_outcome ||
            !DomainOutcomeIsTrue(*decoded.value->domain_outcome) ||
            !DecodeOutput(plan_, *decoded.value->output, output, diagnostic))
            return false;
        output.artifacts = decoded.value->artifacts;
        if (output.outcome == battlecompletion::BattleRecordOutcomeV1::Recorded)
            return output.mismatch_turn == 0 &&
                output.expected_rng == 0 && output.observed_rng == 0 &&
                output.observed_completion.has_value() &&
                output.transition.has_value() &&
                output.artifacts.size() == 2 &&
                output.timing_anchor.dtm_input_index != 0 &&
                output.checkpoint_input_count >=
                    output.timing_anchor.dtm_input_index &&
                output.final_input_count > output.checkpoint_input_count;
        return output.mismatch_turn != 0 && output.artifacts.empty() &&
            output.observed_completion.has_value() ==
                output.transition.has_value();
    }

private:
    std::optional<ProgramInvocation> Resolve(
        const BattleRecordRequestV1& request, ProgramExecutionId execution,
        AttemptId attempt, std::string* diagnostic) const
    {
        if (!execution || !attempt || request.output_dtm_path.empty() ||
            request.output_preseed_savestate_path.empty()) return {};
        auto input = InputGraph(plan_, request, diagnostic);
        if (!input.root) return {};
        return ProgramInvocation{.invocation_id = execution, .attempt_id = attempt,
            .module = module_.identity, .entrypoint = std::string(Entrypoint),
            .dependencies = dependencies_, .runtime_profile = profile_,
            .state = {.policy = InvocationStatePolicy::RestoreBaseline,
                .session_lineage = std::string(BaselineLineage)},
            .execution = runtime_.execution, .input = std::move(input),
            .limits = runtime_.limits,
            .provenance = {.requesting_component = "SavorDb.battle.record",
                .attributes = {{"replay_plan", plan_.canonical_sha256}}}};
    }
    BattleReplayPlanV1 plan_;
    ProgramModule module_;
    ProgramDependencyLock dependencies_;
    RuntimeProfile profile_;
    EncodedModuleEnvelope envelope_;
    fullphase::FullPhaseRuntimeContract runtime_;
    fullphase::FullPhaseProgramIdentity identity_;
};

class KindHandler final : public fullphase::IFullPhaseProgramDefinition
{
public:
    KindHandler()
    {
        BattleReplayPlanV1 sample;
        sample.selected_lineage = {1,1,1,1};
        sample.battle_completion_id = 1;
        sample.turns = {{1, {.fake_attack_count = 0,
            .commands = {{.actor_slot = 0,
                .macro = soa::battle::actions::BattleAction::Attack,
                .params = {.target_slot = 4}}}},
            battlesingleturn::BattleSingleTurnOutcomeV1::Victory, 1}};
        sample.expected_completion.lineage = sample.selected_lineage;
        base_ = std::make_shared<Definition>(std::move(sample));
    }
    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override
    { return base_->identity(); }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override
    { return base_->runtime_contract(); }
    const EncodedModuleEnvelope& module_envelope() const noexcept override
    { return base_->module_envelope(); }
    fullphase::FullPhaseWorksetPolicy workset_policy() const noexcept override
    { return {.minimum_item_count = 1, .maximum_item_count = 1}; }
    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t>, ProgramExecutionId, AttemptId,
        std::string* diagnostic) const override
    {
        Diagnostic(diagnostic, "battle.record requires a prepared workset package");
        return {};
    }
    std::optional<ProgramInvocation> BuildResolvedExecution(
        const fullphase::FullPhaseProgramPackage& package,
        std::span<const std::uint8_t> common,
        std::span<const std::uint8_t> item,
        ProgramExecutionId execution, AttemptId attempt,
        std::string* diagnostic) const override
    {
        BattleReplayPlanV1 plan;
        if (!DecodeBattleReplayPlanV1(common, plan, diagnostic)) return {};
        const auto prepared = PrepareBattleRecordFullPhaseV1(
            std::move(plan), diagnostic);
        if (!prepared ||
            fullphase::BuildFullPhaseProgramPackage(*prepared) != package)
        {
            Diagnostic(diagnostic, "battle.record prepared package identity drifted");
            return {};
        }
        return prepared->BuildResolvedExecution(item, execution, attempt, diagnostic);
    }
private:
    std::shared_ptr<const Definition> base_;
};

class ReplayDefinition final
    : public battlereplay::IBattleReplayFullPhaseDefinitionV1
{
public:
    explicit ReplayDefinition(BattleReplayPlanV1 plan) : plan_(std::move(plan))
    {
        plan_.canonical_sha256 = ComputeBattleReplayPlanHashV1(plan_);
        module_ = ConstructModule(plan_, PhaseFlavor::Replay);
        std::string diagnostic;
        const auto dependencies = Verify(module_, &diagnostic);
        const auto encoded = EncodeProgramModuleV1(module_);
        if (!dependencies || !encoded)
            throw std::logic_error(diagnostic.empty()
                ? encoded.status.message : diagnostic);
        dependencies_ = *dependencies;
        profile_ = Profile(dependencies_);
        envelope_ = {{module_.identity.canonical_id, module_.identity.revision,
            module_.identity.module_hash.ToHex()}, kProgramCodecVersionV1,
            false, encoded.bytes};
        runtime_ = {.module = envelope_.identity,
            .entrypoint = std::string(battlereplay::Entrypoint),
            .dependency_lock_sha256 =
                ComputeProgramDependencyLockHashV1(dependencies_).ToHex(),
            .runtime_profile_sha256 = ProfileHash(profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = {.intent = ExecutionIntent::Live,
                .allow_movie_playback = true,
                .allow_movie_recording = false,
                .allow_input = true,
                .record_trace = false},
            .limits = module_.budgets,
            .baseline_lineage = std::string(battlereplay::BaselineLineage)};
        const auto invocation = Resolve(ProgramExecutionId(1), AttemptId(1),
            &diagnostic);
        if (!invocation)
            throw std::logic_error("battle.replay sample is invalid");
        runtime_.verified_dependency_sha256 =
            ComputeProgramInvocationCompatibilityHashV1(*invocation);
        constexpr std::string_view movie =
            "soa.battle.replay/observe-adopt-or-inactive/v1";
        constexpr std::string_view service =
            "soa.battle.replay/adaptive-battle-completion/v1";
        runtime_.movie_policy_sha256 = hash::sha256(movie.data(), movie.size());
        runtime_.service_policy_sha256 = hash::sha256(service.data(), service.size());
        const std::string canonical =
            std::string(battlereplay::FullPhaseCanonicalId) + '\0' +
            runtime_.module.canonical_hash + '\0' + plan_.canonical_sha256 + '\0' +
            runtime_.verified_dependency_sha256;
        identity_ = {static_cast<std::int32_t>(savor::PK_BattleReplay),
            battlereplay::ProgramVersion,
            std::string(battlereplay::FullPhaseCanonicalId), 1,
            hash::sha256(canonical.data(), canonical.size())};
    }

    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override
    { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override
    { return runtime_; }
    const EncodedModuleEnvelope& module_envelope() const noexcept override
    { return envelope_; }
    fullphase::FullPhaseWorksetPolicy workset_policy() const noexcept override
    { return {.minimum_item_count = 1, .maximum_item_count = 1}; }
    const BattleReplayPlanV1& replay_plan() const noexcept override
    { return plan_; }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> bytes, ProgramExecutionId execution,
        AttemptId attempt, std::string* diagnostic) const override
    {
        battlereplay::BattleReplayRequestV1 request;
        if (!battlereplay::DecodeBattleReplayExecutionInputV1(
                bytes, request, diagnostic)) return {};
        return Resolve(execution, attempt, diagnostic);
    }

    bool DecodeProgramResult(
        std::span<const Byte> bytes,
        battlereplay::BattleReplayResultV1& output,
        std::string* diagnostic) const override
    {
        const auto decoded = DecodeProgramResultV1(bytes);
        if (!decoded || !decoded.value ||
            decoded.value->module != module_.identity ||
            decoded.value->entrypoint != battlereplay::Entrypoint ||
            decoded.value->resolved_dependencies != dependencies_ ||
            decoded.value->infrastructure != ProgramInfrastructureStatus::Completed ||
            decoded.value->cleanup != ProgramCleanupStatus::Clean ||
            decoded.value->session_disposition != SessionDisposition::Clean ||
            !decoded.value->output || !decoded.value->domain_outcome ||
            !DomainOutcomeIsTrue(*decoded.value->domain_outcome) ||
            !DecodeReplayOutput(*decoded.value->output, output, diagnostic))
            return false;
        output.artifacts = decoded.value->artifacts;
        if (!output.artifacts.empty()) return false;
        if (output.outcome == battlereplay::BattleReplayOutcomeV1::Matched)
            return output.mismatch_turn == 0 && output.expected_rng == 0 &&
                output.observed_rng == 0 && output.observed_completion &&
                output.transition;
        return output.mismatch_turn != 0 &&
            output.observed_completion.has_value() ==
                output.transition.has_value();
    }

private:
    std::optional<ProgramInvocation> Resolve(
        ProgramExecutionId execution, AttemptId attempt,
        std::string* diagnostic) const
    {
        if (!execution || !attempt) return {};
        auto input = ReplayInputGraph(plan_, diagnostic);
        if (!input.root) return {};
        return ProgramInvocation{.invocation_id = execution,
            .attempt_id = attempt, .module = module_.identity,
            .entrypoint = std::string(battlereplay::Entrypoint),
            .dependencies = dependencies_, .runtime_profile = profile_,
            .state = {.policy = InvocationStatePolicy::RestoreBaseline,
                .session_lineage = std::string(battlereplay::BaselineLineage)},
            .execution = runtime_.execution, .input = std::move(input),
            .limits = runtime_.limits,
            .provenance = {.requesting_component = "SavorDb.battle.replay",
                .attributes = {{"replay_plan", plan_.canonical_sha256}}}};
    }
    BattleReplayPlanV1 plan_;
    ProgramModule module_;
    ProgramDependencyLock dependencies_;
    RuntimeProfile profile_;
    EncodedModuleEnvelope envelope_;
    fullphase::FullPhaseRuntimeContract runtime_;
    fullphase::FullPhaseProgramIdentity identity_;
};

class ReplayKindHandler final : public fullphase::IFullPhaseProgramDefinition
{
public:
    ReplayKindHandler()
    {
        BattleReplayPlanV1 sample;
        sample.selected_lineage = {1,1,1,1};
        sample.battle_completion_id = 1;
        sample.turns = {{1, {.fake_attack_count = 0,
            .commands = {{.actor_slot = 0,
                .macro = soa::battle::actions::BattleAction::Attack,
                .params = {.target_slot = 4}}}},
            battlesingleturn::BattleSingleTurnOutcomeV1::Victory, 1}};
        sample.expected_completion.lineage = sample.selected_lineage;
        base_ = std::make_shared<ReplayDefinition>(std::move(sample));
    }
    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override
    { return base_->identity(); }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override
    { return base_->runtime_contract(); }
    const EncodedModuleEnvelope& module_envelope() const noexcept override
    { return base_->module_envelope(); }
    fullphase::FullPhaseWorksetPolicy workset_policy() const noexcept override
    { return {.minimum_item_count = 1, .maximum_item_count = 1}; }
    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t>, ProgramExecutionId, AttemptId,
        std::string* diagnostic) const override
    {
        Diagnostic(diagnostic, "battle.replay requires a prepared workset package");
        return {};
    }
    std::optional<ProgramInvocation> BuildResolvedExecution(
        const fullphase::FullPhaseProgramPackage& package,
        std::span<const std::uint8_t> common,
        std::span<const std::uint8_t> item,
        ProgramExecutionId execution, AttemptId attempt,
        std::string* diagnostic) const override
    {
        BattleReplayPlanV1 plan;
        if (!DecodeBattleReplayPlanV1(common, plan, diagnostic)) return {};
        const auto prepared = battlereplay::PrepareBattleReplayFullPhaseV1(
            std::move(plan), diagnostic);
        if (!prepared ||
            fullphase::BuildFullPhaseProgramPackage(*prepared) != package)
        {
            Diagnostic(diagnostic,
                "battle.replay prepared package identity drifted");
            return {};
        }
        return prepared->BuildResolvedExecution(
            item, execution, attempt, diagnostic);
    }
private:
    std::shared_ptr<const ReplayDefinition> base_;
};

} // namespace

std::shared_ptr<const IBattleRecordFullPhaseDefinitionV1>
PrepareBattleRecordFullPhaseV1(
    BattleReplayPlanV1 plan,
    std::string* diagnostic)
{
    plan.canonical_sha256 = ComputeBattleReplayPlanHashV1(plan);
    if (!ValidateBattleReplayPlanV1(plan, diagnostic)) return {};
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<const Definition>> cache;
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(plan.canonical_sha256); found != cache.end())
        if (const auto existing = found->second.lock()) return existing;
    try
    {
        auto created = std::make_shared<Definition>(std::move(plan));
        cache[created->replay_plan().canonical_sha256] = created;
        return created;
    }
    catch (const std::exception& error)
    {
        Diagnostic(diagnostic, error.what()); return {};
    }
}

std::shared_ptr<const fullphase::IFullPhaseProgramDefinition>
BattleRecordKindHandlerV1()
{
    static const auto handler = std::make_shared<const KindHandler>();
    return handler;
}

} // namespace savor::runtime::battlerecord

namespace savor::runtime::battlereplay {

std::vector<std::uint8_t> EncodeBattleReplayExecutionInputV1(
    const BattleReplayRequestV1&)
{
    return {'B', 'R', 'I', '1'};
}

bool DecodeBattleReplayExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleReplayRequestV1& request,
    std::string* diagnostic)
{
    request = {};
    const bool valid = bytes.size() == 4 && bytes[0] == 'B' &&
        bytes[1] == 'R' && bytes[2] == 'I' && bytes[3] == '1';
    if (diagnostic)
        *diagnostic = valid ? std::string{} :
            "battle.replay execution input is not canonical BRI1";
    return valid;
}

std::shared_ptr<const IBattleReplayFullPhaseDefinitionV1>
PrepareBattleReplayFullPhaseV1(
    battlerecord::BattleReplayPlanV1 plan,
    std::string* diagnostic)
{
    plan.canonical_sha256 = battlerecord::ComputeBattleReplayPlanHashV1(plan);
    if (!battlerecord::ValidateBattleReplayPlanV1(plan, diagnostic)) return {};
    static std::mutex mutex;
    static std::map<std::string,
        std::weak_ptr<const battlerecord::ReplayDefinition>> cache;
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(plan.canonical_sha256); found != cache.end())
        if (const auto existing = found->second.lock()) return existing;
    try
    {
        auto created = std::make_shared<battlerecord::ReplayDefinition>(
            std::move(plan));
        cache[created->replay_plan().canonical_sha256] = created;
        return created;
    }
    catch (const std::exception& error)
    {
        if (diagnostic) *diagnostic = error.what();
        return {};
    }
}

std::shared_ptr<const fullphase::IFullPhaseProgramDefinition>
BattleReplayKindHandlerV1()
{
    static const auto handler =
        std::make_shared<const battlerecord::ReplayKindHandler>();
    return handler;
}

} // namespace savor::runtime::battlereplay
