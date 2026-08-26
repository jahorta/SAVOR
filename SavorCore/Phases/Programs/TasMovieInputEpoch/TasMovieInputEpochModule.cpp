#include "TasMovieInputEpochModule.h"

#include "Runner/Runtime/Execution/ExecutionTypes.h"
#include "Runner/Runtime/ProgramKind.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.h"
#include "Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <utility>

namespace savor::runtime::tasmovie::inputepoch {
namespace {

using namespace program;
using namespace program::composition;
using Builder = program::composition::detail::ModuleFragmentBuilder;

void Diagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

SchemaIdentity Schema(std::string id, std::string contract)
{
    return ExactSchema(std::move(id), 1, contract);
}

SchemaIdentity HashSchema()
{
    return Schema("soa.tasmovie.input_epoch.Sha256",
        "utf8(max=64;lowercase sha256)");
}

SchemaIdentity EpochSchema()
{
    return Schema("soa.tasmovie.input_epoch.EpochV1",
        "record{movie_input_cursor:u64,input:runtime.input.InputFramePayload}");
}

SchemaIdentity EpochListSchema()
{
    return Schema("soa.tasmovie.input_epoch.EpochListV1",
        "list(max=1000000;soa.tasmovie.input_epoch.EpochV1/1)");
}

SchemaIdentity OutcomeSchema()
{
    return Schema("soa.tasmovie.input_epoch.OutcomeV1",
        "enum{Completed=0,Diverged=1}");
}

SchemaIdentity FailureSchema()
{
    return Schema("soa.tasmovie.input_epoch.FailureReasonV1",
        "enum{None=0,CursorZero=1,CursorRegressed=2,CursorPastSource=3,UnexpectedMovieEnd=4,PrefixDiverged=5}");
}

SchemaIdentity AnnotationRequestSchema()
{
    return Schema("soa.tasmovie.input_epoch.AnnotationRequestV1",
        "record{prepare:runtime.movie.prepare_read_only_playback.Input/1,source_hash:Sha256/1,source_poll_count:u64}");
}

SchemaIdentity AnnotationResultSchema()
{
    return Schema("soa.tasmovie.input_epoch.AnnotationResultV1",
        "record{outcome:OutcomeV1/1,source_hash:Sha256/1,source_poll_count:u64,epochs:EpochListV1/1,failure:FailureReasonV1/1,failure_epoch:u64,expected_cursor:u64,actual_cursor:u64}");
}

SchemaIdentity RewriteRequestSchema()
{
    return Schema("soa.tasmovie.input_epoch.RewriteRequestV1",
        "record{prepare:runtime.movie.prepare_read_only_playback.Input/1,epochs:EpochListV1/1,insert_before:u64,recording_config:runtime.movie.RecordingStaticConfig,save_request:runtime.action.savestate_save_immutable_artifact.Input/1}");
}

SchemaIdentity RewriteResultSchema()
{
    return Schema("soa.tasmovie.input_epoch.RewriteResultV1",
        "record{outcome:OutcomeV1/1,insert_before:u64,source_count:u64,child_count:u64,final_cursor:u64,failure:FailureReasonV1/1,failure_epoch:u64,expected_cursor:u64,actual_cursor:u64}");
}

TypeRef Named(const SchemaIdentity& schema) { return TypeRef::Named(schema); }
TypeRef U64() { return TypeRef::Builtin(BuiltinType::U64); }
TypeRef Bool() { return TypeRef::Builtin(BuiltinType::Bool); }

InstructionTarget Action(CanonicalAction action)
{
    return {.kind = InstructionTargetKind::Action,
        .dependency = CanonicalActionIdentity(action)};
}

ProgramValueId Need(std::optional<ProgramValueId> value, std::string_view what)
{
    if (!value) throw std::logic_error("input-epoch lowering failed: " +
        std::string(what));
    return *value;
}

BasicBlock& Block(ProgramFunction& function, ProgramBlockId id)
{
    const auto found = std::ranges::find(function.blocks, id, &BasicBlock::id);
    if (found == function.blocks.end())
        throw std::logic_error("input-epoch block is unavailable");
    return *found;
}

void AddAction(Builder& builder, CanonicalAction action)
{
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const auto& schema : CanonicalActionTypeSchemaClosure(action))
        builder.AddTypeImport(schema);
}

ProgramValueId Constant(Builder& builder, ProgramFunction& function,
    BasicBlock& block, TypeRef type, LiteralPayload payload,
    std::string selector, ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::Constant, type, {}, {}, std::move(selector),
        LiteralValue{type, std::move(payload)}, scope), "constant");
}

ProgramValueId Construct(Builder& builder, ProgramFunction& function,
    BasicBlock& block, TypeRef type, std::span<const ProgramValueId> fields,
    std::string selector, ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::RecordConstruct, type, fields, {},
        std::move(selector), std::nullopt, scope), "record");
}

ProgramValueId Project(Builder& builder, ProgramFunction& function,
    BasicBlock& block, ProgramValueId value, TypeRef type,
    std::string field, ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::RecordProject, type, std::array{value}, {},
        std::move(field), std::nullopt, scope), "projection");
}

ProgramValueId Binary(Builder& builder, ProgramFunction& function,
    BasicBlock& block, InstructionOpcode opcode, TypeRef type,
    ProgramValueId left, ProgramValueId right, std::string selector,
    ProgramScopeId scope = {})
{
    return Need(builder.AddInstruction(function, block, opcode, type,
        std::array{left, right}, {}, std::move(selector), std::nullopt, scope),
        "binary operation");
}

ProgramValueId OptionalValue(Builder& builder, ProgramFunction& function,
    BasicBlock& block, CanonicalRuntimeSchema schema,
    std::optional<ProgramValueId> value, std::string selector,
    ProgramScopeId scope)
{
    const std::array<ProgramValueId, 1> present{value.value_or(ProgramValueId{})};
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::OptionalConstruct, CanonicalRuntimeType(schema),
        value ? std::span<const ProgramValueId>(present) :
                std::span<const ProgramValueId>{}, {}, std::move(selector),
        std::nullopt, scope), "optional");
}

ProgramValueId Await(Builder& builder, ProgramFunction& function,
    BasicBlock& block, CanonicalAction action, ProgramValueId request,
    std::string selector, ProgramScopeId scope = {})
{
    const bool creates_resource =
        action == CanonicalAction::MoviePrepareReadOnlyPlayback
        || action == CanonicalAction::MovieStartPlayback
        || action == CanonicalAction::MovieStartRecording
        || action == CanonicalAction::InputAcquireLease;
    return Need(builder.AddInstruction(function, block,
        InstructionOpcode::AwaitAction, CanonicalActionOutputType(action),
        std::array{request}, Action(action), std::move(selector),
        std::nullopt, creates_resource ? scope : ProgramScopeId{}), "action");
}

class StaticWriter
{
public:
    explicit StaticWriter(std::array<char, 4> magic)
    {
        for (const char ch : magic) bytes_.push_back(static_cast<Byte>(ch));
    }
    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<Byte>(value >> shift));
    }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Bool(bool value) { U8(value ? 1 : 0); }
    std::vector<Byte> Finish() && { return std::move(bytes_); }
private:
    std::vector<Byte> bytes_;
};

std::vector<Byte> ContinueConfig(bool fail_on_movie_end)
{
    StaticWriter writer({'C','U','C','1'});
    writer.U8(1);
    writer.Bool(true);
    writer.Bool(fail_on_movie_end);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> PadStatusObservationConfig()
{
    StaticWriter writer({'O','S','C','1'});
    writer.Text("tasmovie-pad-status");
    writer.U8(0);
    writer.Bool(false);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> LeaseConfig()
{
    StaticWriter writer({'I','L','C','2'});
    writer.U32(0);
    writer.U32(0);
    writer.Bool(true);
    writer.Bool(true);
    writer.Bool(false);
    return std::move(writer).Finish();
}

std::vector<Byte> AdvanceConfig()
{
    StaticWriter writer({'E','A','C','1'});
    writer.U8(2);
    writer.Bool(false);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> FrameBytes(const GCInputFrame& frame)
{
    return {static_cast<Byte>(frame.buttons),
        static_cast<Byte>(frame.buttons >> 8u), frame.main_x, frame.main_y,
        frame.c_x, frame.c_y, frame.trig_l, frame.trig_r};
}

std::vector<Byte> PointSet()
{
    const std::array<SemanticPointReference, 1> points{{{
        .capability_pack = capabilities::FieldPackIdentity(),
        .canonical_id = std::string(PadReadReturnedPointId),
        .kind = SemanticPointKind::ProgramCounter,
        .physical_pc = PadReadReturnedPc,
    }}};
    return EncodeSemanticPointSetV1(points);
}

void AddTypes(Builder& builder)
{
    builder.AddTypeImport(CanonicalRuntimeSchemaIdentity(
        CanonicalRuntimeSchema::InputFramePayload));
    builder.AddLocalType({.identity = HashSchema(),
        .kind = TypeSchemaKind::BoundedUtf8String, .maximum_size = 64});
    builder.AddLocalType({.identity = EpochSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"movie_input_cursor", U64()},
            {"input", CanonicalRuntimeType(
                CanonicalRuntimeSchema::InputFramePayload)}}});
    builder.AddLocalType({.identity = EpochListSchema(),
        .kind = TypeSchemaKind::BoundedList, .maximum_size = MaximumEpochs,
        .element_type = Named(EpochSchema())});
    builder.AddLocalType({.identity = OutcomeSchema(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {{"Completed", 0}, {"Diverged", 1}}});
    builder.AddLocalType({.identity = FailureSchema(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {{"None", 0}, {"CursorZero", 1},
            {"CursorRegressed", 2}, {"CursorPastSource", 3},
            {"UnexpectedMovieEnd", 4}, {"PrefixDiverged", 5}}});
}

ProgramValueId EnumConstant(Builder& builder, ProgramFunction& function,
    BasicBlock& block, const SchemaIdentity& schema, std::int64_t value,
    std::string selector, ProgramScopeId scope = {})
{
    return Constant(builder, function, block, Named(schema),
        EnumValue{schema, value}, std::move(selector), scope);
}

ProgramValueId Continue(Builder& builder, ProgramFunction& function,
    BasicBlock& block, ProgramValueId playback,
    std::optional<ProgramValueId> binding, bool fail_on_movie_end,
    std::string selector, ProgramScopeId scope)
{
    const auto points = Constant(builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::SemanticPointSet),
        PointSet(), selector + "/points", scope);
    const auto input = OptionalValue(builder, function, block,
        CanonicalRuntimeSchema::OptionalInputExecutionBinding, binding,
        selector + "/input", scope);
    const auto movie = OptionalValue(builder, function, block,
        CanonicalRuntimeSchema::OptionalMoviePlaybackSession,
        playback ? std::optional<ProgramValueId>(playback) : std::nullopt,
        selector + "/movie", scope);
    const auto count = OptionalValue(builder, function, block,
        CanonicalRuntimeSchema::OptionalMovieInputCount, std::nullopt,
        selector + "/count", scope);
    const auto config = Constant(builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ContinueUntilStaticConfig),
        ContinueConfig(fail_on_movie_end), selector + "/config", scope);
    const auto request = Construct(builder, function, block,
        CanonicalActionInputType(CanonicalAction::ExecutionContinueUntil),
        std::array{points, input, movie, count, config},
        selector + "/request", scope);
    return Await(builder, function, block,
        CanonicalAction::ExecutionContinueUntil, request,
        selector + "/await", scope);
}

void FailBlock(Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramScopeId scope, std::string code, std::string message)
{
    (void)builder.AddInstruction(function, block, InstructionOpcode::ExitScope,
        std::nullopt, {}, {}, "release-scope", std::nullopt, scope);
    builder.SetTerminator(function, block, {.kind = TerminatorKind::StructuredFail,
        .failure = StructuredFailure{std::move(code), std::move(message),
            std::nullopt}}, "failure");
}

ProgramModule AnnotationModule()
{
    ProgramModule module{.identity = {
        .canonical_id = std::string(AnnotationModuleCanonicalId),
        .revision = 1}};
    Builder builder(module, "TasMovieInputEpochAnnotation",
        "tasmovie.annotate_input_epochs/v1");
    AddTypes(builder);
    builder.AddLocalType({.identity = AnnotationRequestSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"prepare", CanonicalActionInputType(
                CanonicalAction::MoviePrepareReadOnlyPlayback)},
            {"source_hash", Named(HashSchema())},
            {"source_poll_count", U64()}}});
    builder.AddLocalType({.identity = AnnotationResultSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"outcome", Named(OutcomeSchema())},
            {"source_hash", Named(HashSchema())}, {"source_poll_count", U64()},
            {"epochs", Named(EpochListSchema())},
            {"failure", Named(FailureSchema())},
            {"failure_epoch", U64()}, {"expected_cursor", U64()},
            {"actual_cursor", U64()}}});
    for (const auto action : {CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::GuestReadU32,
             CanonicalAction::GuestReadU64}) AddAction(builder, action);
    builder.AddCapabilityImport(capabilities::FieldPackIdentity());
    builder.AddReducerImport(
        capabilities::FieldPadStatusToInputFrameReducerIdentity());

    const auto argument = builder.NewArgument(Named(AnnotationRequestSchema()));
    auto& function = builder.AddFunction(std::string(AnnotationEntrypoint),
        std::array{argument}, Named(AnnotationResultSchema()), Bool(), true);
    function.blocks.reserve(16);
    const auto entry_id = builder.AddBlock(function).id;
    const auto loop_args = std::array{builder.NewArgument(Named(EpochListSchema())),
        builder.NewArgument(U64())};
    const auto loop_id = builder.AddBlock(function, loop_args).id;
    const auto hit_args = std::array{
        builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil)),
        builder.NewArgument(Named(EpochListSchema())),
        builder.NewArgument(U64())};
    const auto hit_id = builder.AddBlock(function, hit_args).id;
    const auto ended_args =
        std::array{builder.NewArgument(Named(EpochListSchema()))};
    const auto ended_id = builder.AddBlock(function, ended_args).id;
    const auto fail_zero_id = builder.AddBlock(function).id;
    const auto fail_regress_id = builder.AddBlock(function).id;
    const auto fail_past_id = builder.AddBlock(function).id;
    const auto fail_overrun_id = builder.AddBlock(function).id;
    const auto regress_args = std::array{builder.NewArgument(U64()),
        builder.NewArgument(U64()),
        builder.NewArgument(Named(EpochListSchema())),
        builder.NewArgument(U64()), builder.NewArgument(Bool()),
        builder.NewArgument(Bool()),
        builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))};
    const auto regress_check_id = builder.AddBlock(function, regress_args).id;
    const auto past_args = std::array{builder.NewArgument(U64()),
        builder.NewArgument(U64()),
        builder.NewArgument(Named(EpochListSchema())),
        builder.NewArgument(U64()), builder.NewArgument(Bool()),
        builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))};
    const auto past_check_id = builder.AddBlock(function, past_args).id;

    auto& entry = Block(function, entry_id);
    const auto scope = builder.NewScope();
    (void)builder.AddInstruction(function, entry, InstructionOpcode::EnterScope,
        std::nullopt, {}, {}, "scope", std::nullopt, scope);
    const auto prepare_request = Project(builder, function, entry, argument.id,
        CanonicalActionInputType(CanonicalAction::MoviePrepareReadOnlyPlayback),
        "prepare", scope);
    const auto prepared = Await(builder, function, entry,
        CanonicalAction::MoviePrepareReadOnlyPlayback, prepare_request,
        "prepare", scope);
    const auto playback = Await(builder, function, entry,
        CanonicalAction::MovieStartPlayback, prepared, "start", scope);
    const auto empty = Need(builder.AddInstruction(function, entry,
        InstructionOpcode::ListConstruct, Named(EpochListSchema()), {}, {},
        "empty-epochs", std::nullopt, scope), "empty epoch list");
    const auto zero = Constant(builder, function, entry, U64(),
        std::uint64_t{0}, "zero", scope);
    builder.SetTerminator(function, entry, {.kind = TerminatorKind::Branch,
        .edges = {{loop_id, {empty, zero}}}}, "begin-loop");

    auto& loop = Block(function, loop_id);
    const auto observation = Continue(builder, function, loop, playback,
        std::nullopt, false, "next-pad-read", scope);
    const auto reason = Project(builder, function, loop, observation,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        "reason", scope);
    builder.SetTerminator(function, loop, {.kind = TerminatorKind::EnumSwitch,
        .condition_or_selector = reason,
        .enum_cases = {{static_cast<std::int64_t>(
                ContinueUntilCompletionReasonV1::Breakpoint),
                {hit_id, {observation, loop_args[0].id,
                    loop_args[1].id}}},
            {static_cast<std::int64_t>(
                ContinueUntilCompletionReasonV1::MovieEnded),
                {ended_id, {loop_args[0].id}}},
            {static_cast<std::int64_t>(
                ContinueUntilCompletionReasonV1::CursorOverrun),
                {fail_overrun_id, {}}}}}, "classify");

    auto& hit = Block(function, hit_id);
    const auto cursor = Project(builder, function, hit, hit_args[0].id,
        U64(), "movie_input_count", scope);
    const auto source_count = Project(builder, function, hit, argument.id,
        U64(), "source_poll_count", scope);
    const auto is_zero = Binary(builder, function, hit, InstructionOpcode::Equal,
        Bool(), cursor, Constant(builder, function, hit, U64(),
            std::uint64_t{0}, "zero", scope), "cursor-zero", scope);
    const auto regressed = Binary(builder, function, hit, InstructionOpcode::Less,
        Bool(), cursor, hit_args[2].id, "cursor-regressed", scope);
    const auto past = Binary(builder, function, hit, InstructionOpcode::Greater,
        Bool(), cursor, source_count, "cursor-past-source", scope);
    const auto valid_id = builder.AddBlock(function,
        std::array{builder.NewArgument(U64()),
            builder.NewArgument(U64()),
            builder.NewArgument(Named(EpochListSchema())),
            builder.NewArgument(CanonicalActionOutputType(
                CanonicalAction::ExecutionContinueUntil))}).id;
    builder.SetTerminator(function, hit,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = is_zero,
         .edges = {{fail_zero_id, {}}, {regress_check_id, {cursor, source_count,
             hit_args[1].id, hit_args[2].id, regressed, past,
             hit_args[0].id}}}},
        "validate-cursor-zero");

    auto& regress_check = Block(function, regress_check_id);
    builder.SetTerminator(function, regress_check,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = regress_check.arguments[4].id,
         .edges = {{fail_regress_id, {}}, {past_check_id,
             {regress_check.arguments[0].id, regress_check.arguments[1].id,
              regress_check.arguments[2].id, regress_check.arguments[3].id,
              regress_check.arguments[5].id,
              regress_check.arguments[6].id}}}},
        "validate-cursor-regression");

    auto& past_check = Block(function, past_check_id);
    builder.SetTerminator(function, past_check,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = past_check.arguments[4].id,
         .edges = {{fail_past_id, {}}, {valid_id,
             {past_check.arguments[0].id, past_check.arguments[1].id,
              past_check.arguments[2].id,
              past_check.arguments[5].id}}}},
        "validate-cursor-past-source");

    auto& valid = Block(function, valid_id);
    const auto stop = OptionalValue(builder, function, valid,
        CanonicalRuntimeSchema::OptionalContinueUntilResult,
        valid.arguments[3].id, "pad-read-stop", scope);
    const auto config = Constant(builder, function, valid,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ObservationStaticConfig),
        PadStatusObservationConfig(), "pad-status-config", scope);
    const auto pointer_address = Constant(builder, function, valid, U64(),
        std::uint64_t{0x8034763c}, "pad-status-pointer-address", scope);
    const auto pointer_request = Construct(builder, function, valid,
        CanonicalActionInputType(CanonicalAction::GuestReadU32),
        std::array{stop, pointer_address, config}, "pad-status-pointer-request",
        scope);
    const auto pointer_u32 = Await(builder, function, valid,
        CanonicalAction::GuestReadU32, pointer_request, "pad-status-pointer",
        scope);
    const auto pointer = Need(builder.AddInstruction(function, valid,
        InstructionOpcode::CheckedConvert, U64(), std::array{pointer_u32}, {},
        "pad-status-pointer-widen", std::nullopt, scope),
        "pad status pointer widening");
    const auto status_request = Construct(builder, function, valid,
        CanonicalActionInputType(CanonicalAction::GuestReadU64),
        std::array{stop, pointer, config}, "pad-status-request", scope);
    const auto packed_status = Await(builder, function, valid,
        CanonicalAction::GuestReadU64, status_request, "pad-status", scope);
    const auto frame = Need(builder.AddInstruction(function, valid,
        InstructionOpcode::CallReducer,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        std::array{packed_status},
        {.kind = InstructionTargetKind::Reducer,
         .dependency = capabilities::FieldPadStatusToInputFrameReducerIdentity()},
        "canonicalize-pad-status", std::nullopt, scope),
        "PADStatus canonicalization");
    const auto epoch = Construct(builder, function, valid, Named(EpochSchema()),
        std::array{valid.arguments[0].id, frame}, "epoch", scope);
    const auto appended = Need(builder.AddInstruction(function, valid,
        InstructionOpcode::ListAppend, Named(EpochListSchema()),
        std::array{valid.arguments[2].id, epoch}, {}, "append-epoch",
        std::nullopt, scope), "append epoch");
    builder.SetTerminator(function, valid, {.kind = TerminatorKind::Branch,
        .edges = {{loop_id, {appended, valid.arguments[0].id}}}},
        "continue-annotation");

    auto& ended = Block(function, ended_id);
    const auto source_hash = Project(builder, function, ended, argument.id,
        Named(HashSchema()), "source_hash", scope);
    const auto ended_count = Project(builder, function, ended, argument.id,
        U64(), "source_poll_count", scope);
    const auto outcome = EnumConstant(builder, function, ended,
        OutcomeSchema(), 0, "completed", scope);
    const auto none = EnumConstant(builder, function, ended,
        FailureSchema(), 0, "no-failure", scope);
    const auto z = Constant(builder, function, ended, U64(),
        std::uint64_t{0}, "zero", scope);
    const auto result = Construct(builder, function, ended,
        Named(AnnotationResultSchema()), std::array{outcome, source_hash,
            ended_count, ended_args[0].id, none, z, z, z}, "result", scope);
    (void)builder.AddInstruction(function, ended, InstructionOpcode::ExitScope,
        std::nullopt, {}, {}, "release-scope", std::nullopt, scope);
    const auto succeeded = Constant(builder, function, ended, Bool(), true,
        "succeeded");
    builder.SetTerminator(function, ended, {.kind = TerminatorKind::Return,
        .return_value = result, .domain_outcome = succeeded}, "return");

    FailBlock(builder, function, Block(function, fail_zero_id), scope,
        "tasmovie.input_epoch.cursor_invalid",
        "movie input cursor was zero, regressed, or exceeded the source poll stream");
    FailBlock(builder, function, Block(function, fail_regress_id), scope,
        "tasmovie.input_epoch.cursor_regressed", "movie input cursor regressed");
    FailBlock(builder, function, Block(function, fail_past_id), scope,
        "tasmovie.input_epoch.cursor_past_source", "movie cursor exceeded source polls");
    FailBlock(builder, function, Block(function, fail_overrun_id), scope,
        "tasmovie.input_epoch.cursor_overrun", "movie cursor overrun was reported");

    module.accepted_policies = {.state_policies = {
            InvocationStatePolicy::EstablishBaseline},
        .execution_intents = {ExecutionIntent::Live},
        .permits_movie_playback = true};
    module.budgets = {.maximum_instructions = 12'000'000,
        .maximum_calls = 64, .maximum_call_depth = 8,
        .maximum_action_requests = 2'000'010, .maximum_emissions = 1,
        .maximum_artifacts = 1, .maximum_values = 16'000'000,
        .maximum_value_bytes = 512ull * 1024ull * 1024ull,
        .maximum_trace_events = 2'000'000};
    module.entrypoints = {{.name = std::string(AnnotationEntrypoint),
        .function = function.id, .input_type = Named(AnnotationRequestSchema()),
        .output_type = Named(AnnotationResultSchema()),
        .domain_outcome_type = Bool(),
        .required_capability_packs = module.required_capability_packs,
        .accepted_policies = module.accepted_policies}};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

// Rewrite uses the same canonical action vocabulary. The IR is intentionally
// compact: prefix verification and held-input delivery are performed by two
// local loops, and every backend mutation remains an awaited canonical action.
ProgramModule RewriteModule()
{
    ProgramModule module{.identity = {
        .canonical_id = std::string(RewriteModuleCanonicalId), .revision = 1}};
    Builder builder(module, "TasMovieInputEpochRewrite",
        "tasmovie.rewrite_input_epochs/v1");
    AddTypes(builder);
    builder.AddLocalType({.identity = RewriteRequestSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"prepare", CanonicalActionInputType(
                CanonicalAction::MoviePrepareReadOnlyPlayback)},
            {"epochs", Named(EpochListSchema())}, {"insert_before", U64()},
            {"recording_config", CanonicalRuntimeType(
                CanonicalRuntimeSchema::MovieRecordingStaticConfig)},
            {"save_request", CanonicalActionInputType(
                CanonicalAction::SavestateSaveImmutableArtifact)}}});
    builder.AddLocalType({.identity = RewriteResultSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {{"outcome", Named(OutcomeSchema())},
            {"insert_before", U64()}, {"source_count", U64()},
            {"child_count", U64()}, {"final_cursor", U64()},
            {"failure", Named(FailureSchema())}, {"failure_epoch", U64()},
            {"expected_cursor", U64()}, {"actual_cursor", U64()}}});
    for (const auto action : {CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::MovieStartRecording,
             CanonicalAction::InputAcquireLease,
             CanonicalAction::InputBeginDelivery,
             CanonicalAction::InputCompleteDelivery,
             CanonicalAction::SavestateSaveImmutableArtifact,
             CanonicalAction::MovieObserveState,
             CanonicalAction::ExecutionContinueUntilInputObserved,
             CanonicalAction::MovieStopRecording}) AddAction(builder, action);
    builder.AddCapabilityImport(capabilities::FieldPackIdentity());

    const auto argument = builder.NewArgument(Named(RewriteRequestSchema()));
    auto& function = builder.AddFunction(std::string(RewriteEntrypoint),
        std::array{argument}, Named(RewriteResultSchema()), Bool(), true);
    function.blocks.reserve(18);
    const auto entry_id = builder.AddBlock(function).id;
    const auto prefix_args = std::array{builder.NewArgument(U64())};
    const auto prefix_id = builder.AddBlock(function, prefix_args).id;
    const auto prefix_hit_args = std::array{
        builder.NewArgument(CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil)),
        builder.NewArgument(U64())};
    const auto prefix_hit_id = builder.AddBlock(function, prefix_hit_args).id;
    const auto branch_id = builder.AddBlock(function).id;
    const auto emit_args = std::array{builder.NewArgument(U64())};
    const auto emit_id = builder.AddBlock(function, emit_args).id;
    const auto neutral_id = builder.AddBlock(function,
        std::array{builder.NewArgument(U64())}).id;
    const auto source_id = builder.AddBlock(function,
        std::array{builder.NewArgument(U64())}).id;
    const auto deliver_args = std::array{
        builder.NewArgument(CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload)),
        builder.NewArgument(U64())};
    const auto deliver_id = builder.AddBlock(function, deliver_args).id;
    const auto finalize_id = builder.AddBlock(function).id;
    const auto fail_prefix_id = builder.AddBlock(function).id;
    const auto fail_movie_id = builder.AddBlock(function).id;

    auto& entry = Block(function, entry_id);
    const auto scope = builder.NewScope();
    (void)builder.AddInstruction(function, entry, InstructionOpcode::EnterScope,
        std::nullopt, {}, {}, "movie-scope", std::nullopt, scope);
    const auto prepare_request = Project(builder, function, entry, argument.id,
        CanonicalActionInputType(CanonicalAction::MoviePrepareReadOnlyPlayback),
        "prepare", scope);
    const auto prepared = Await(builder, function, entry,
        CanonicalAction::MoviePrepareReadOnlyPlayback, prepare_request,
        "prepare", scope);
    const auto playback = Await(builder, function, entry,
        CanonicalAction::MovieStartPlayback, prepared, "start", scope);
    const auto zero = Constant(builder, function, entry, U64(),
        std::uint64_t{0}, "zero", scope);
    builder.SetTerminator(function, entry, {.kind = TerminatorKind::Branch,
        .edges = {{prefix_id, {zero}}}}, "prefix");

    auto& prefix = Block(function, prefix_id);
    const auto insertion = Project(builder, function, prefix, argument.id,
        U64(), "insert_before", scope);
    const auto prefix_done = Binary(builder, function, prefix,
        InstructionOpcode::GreaterEqual, Bool(), prefix_args[0].id, insertion,
        "prefix-done", scope);
    const auto prefix_continue_id = builder.AddBlock(function,
        std::array{builder.NewArgument(U64())}).id;
    builder.SetTerminator(function, prefix,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = prefix_done,
         .edges = {{branch_id, {}}, {prefix_continue_id,
             {prefix_args[0].id}}}}, "prefix-check");
    auto& prefix_continue = Block(function, prefix_continue_id);
    const auto prefix_observation = Continue(builder, function,
        prefix_continue, playback, std::nullopt, true, "prefix-pad-read", scope);
    const auto prefix_reason = Project(builder, function, prefix_continue,
        prefix_observation, CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        "reason", scope);
    builder.SetTerminator(function, prefix_continue,
        {.kind = TerminatorKind::EnumSwitch,
         .condition_or_selector = prefix_reason,
         .enum_cases = {{static_cast<std::int64_t>(
             ContinueUntilCompletionReasonV1::Breakpoint),
             {prefix_hit_id, {prefix_observation,
                 prefix_continue.arguments[0].id}}}},
         .default_edge = {{fail_movie_id, {}}}}, "prefix-observation");

    auto& prefix_hit = Block(function, prefix_hit_id);
    const auto epochs = Project(builder, function, prefix_hit, argument.id,
        Named(EpochListSchema()), "epochs", scope);
    const auto expected_epoch = Need(builder.AddInstruction(function,
        prefix_hit, InstructionOpcode::ListIndex, Named(EpochSchema()),
        std::array{epochs, prefix_hit_args[1].id}, {}, "expected-epoch",
        std::nullopt, scope), "expected epoch");
    const auto expected_cursor = Project(builder, function, prefix_hit,
        expected_epoch, U64(), "movie_input_cursor", scope);
    const auto actual_cursor = Project(builder, function, prefix_hit,
        prefix_hit_args[0].id, U64(), "movie_input_count", scope);
    const auto matches = Binary(builder, function, prefix_hit,
        InstructionOpcode::Equal, Bool(), expected_cursor, actual_cursor,
        "prefix-matches", scope);
    const auto one = Constant(builder, function, prefix_hit, U64(),
        std::uint64_t{1}, "one", scope);
    const auto next_prefix = Binary(builder, function, prefix_hit,
        InstructionOpcode::AddChecked, U64(), prefix_hit_args[1].id, one,
        "next-prefix", scope);
    builder.SetTerminator(function, prefix_hit,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = matches,
         .edges = {{prefix_id, {next_prefix}}, {fail_prefix_id, {}}}},
        "verify-prefix");

    auto& branch = Block(function, branch_id);
    const auto recording_config = Project(builder, function, branch,
        argument.id, CanonicalRuntimeType(
            CanonicalRuntimeSchema::MovieRecordingStaticConfig),
        "recording_config", scope);
    const auto recording_request = Construct(builder, function, branch,
        CanonicalActionInputType(CanonicalAction::MovieStartRecording),
        std::array{playback, recording_config}, "recording-request", scope);
    const auto recording = Await(builder, function, branch,
        CanonicalAction::MovieStartRecording, recording_request,
        "start-recording", scope);
    const auto lease_config = Constant(builder, function, branch,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputLeaseStaticConfig),
        LeaseConfig(), "lease-config", scope);
    const auto lease_request = Construct(builder, function, branch,
        CanonicalActionInputType(CanonicalAction::InputAcquireLease),
        std::array{lease_config}, "lease-request", scope);
    const auto lease = Await(builder, function, branch,
        CanonicalAction::InputAcquireLease, lease_request, "lease", scope);
    builder.SetTerminator(function, branch, {.kind = TerminatorKind::Branch,
        .edges = {{emit_id, {zero}}}}, "emit");

    auto& emit = Block(function, emit_id);
    const auto emit_epochs = Project(builder, function, emit, argument.id,
        Named(EpochListSchema()), "epochs", scope);
    const auto source_count = Need(builder.AddInstruction(function, emit,
        InstructionOpcode::ListSize, U64(), std::array{emit_epochs}, {},
        "source-count", std::nullopt, scope), "source count");
    const auto emit_insert = Project(builder, function, emit, argument.id,
        U64(), "insert_before", scope);
    const auto suffix_count = Binary(builder, function, emit,
        InstructionOpcode::SubtractChecked, U64(), source_count, emit_insert,
        "suffix-count", scope);
    const auto emit_one = Constant(builder, function, emit, U64(),
        std::uint64_t{1}, "one", scope);
    const auto child_count = Binary(builder, function, emit,
        InstructionOpcode::AddChecked, U64(), suffix_count, emit_one,
        "child-count", scope);
    const auto emit_done = Binary(builder, function, emit,
        InstructionOpcode::GreaterEqual, Bool(), emit_args[0].id, child_count,
        "emit-done", scope);
    const auto is_neutral = Binary(builder, function, emit,
        InstructionOpcode::Equal, Bool(), emit_args[0].id,
        Constant(builder, function, emit, U64(), std::uint64_t{0},
            "zero", scope), "is-neutral", scope);
    const auto choose_id = builder.AddBlock(function,
        std::array{builder.NewArgument(U64()), builder.NewArgument(Bool())}).id;
    builder.SetTerminator(function, emit,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = emit_done,
         .edges = {{finalize_id, {}}, {choose_id,
             {emit_args[0].id, is_neutral}}}}, "emit-check");
    auto& choose = Block(function, choose_id);
    builder.SetTerminator(function, choose,
        {.kind = TerminatorKind::ConditionalBranch,
         .condition_or_selector = choose.arguments[1].id,
         .edges = {{neutral_id, {choose.arguments[0].id}},
             {source_id, {choose.arguments[0].id}}}}, "choose-frame");

    auto& neutral = Block(function, neutral_id);
    const auto neutral_frame = Constant(builder, function, neutral,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        FrameBytes(GCInputFrame{}), "neutral", scope);
    builder.SetTerminator(function, neutral, {.kind = TerminatorKind::Branch,
        .edges = {{deliver_id, {neutral_frame, neutral.arguments[0].id}}}},
        "deliver-neutral");

    auto& source = Block(function, source_id);
    const auto source_epochs = Project(builder, function, source, argument.id,
        Named(EpochListSchema()), "epochs", scope);
    const auto source_insert = Project(builder, function, source, argument.id,
        U64(), "insert_before", scope);
    const auto source_one = Constant(builder, function, source, U64(),
        std::uint64_t{1}, "one", scope);
    const auto suffix_index = Binary(builder, function, source,
        InstructionOpcode::SubtractChecked, U64(), source.arguments[0].id,
        source_one, "suffix-index", scope);
    const auto source_index = Binary(builder, function, source,
        InstructionOpcode::AddChecked, U64(), source_insert, suffix_index,
        "source-index", scope);
    const auto source_epoch = Need(builder.AddInstruction(function, source,
        InstructionOpcode::ListIndex, Named(EpochSchema()),
        std::array{source_epochs, source_index}, {}, "source-epoch",
        std::nullopt, scope), "source epoch");
    const auto source_frame = Project(builder, function, source, source_epoch,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        "input", scope);
    builder.SetTerminator(function, source, {.kind = TerminatorKind::Branch,
        .edges = {{deliver_id, {source_frame, source.arguments[0].id}}}},
        "deliver-source");

    auto& deliver = Block(function, deliver_id);
    const auto begin_request = Construct(builder, function, deliver,
        CanonicalActionInputType(CanonicalAction::InputBeginDelivery),
        std::array{lease, deliver_args[0].id}, "begin-request", scope);
    const auto binding = Await(builder, function, deliver,
        CanonicalAction::InputBeginDelivery, begin_request, "begin", scope);
    const auto stop = Continue(builder, function, deliver, ProgramValueId{},
        binding, true, "guest-acknowledgement", scope);
    (void)stop;
    const auto complete_request = Construct(builder, function, deliver,
        CanonicalActionInputType(CanonicalAction::InputCompleteDelivery),
        std::array{lease, binding}, "complete-request", scope);
    (void)Await(builder, function, deliver,
        CanonicalAction::InputCompleteDelivery, complete_request,
        "complete", scope);
    const auto next_emit = Binary(builder, function, deliver,
        InstructionOpcode::AddChecked, U64(), deliver_args[1].id,
        Constant(builder, function, deliver, U64(), std::uint64_t{1},
            "one", scope), "next-emit", scope);
    builder.SetTerminator(function, deliver, {.kind = TerminatorKind::Branch,
        .edges = {{emit_id, {next_emit}}}}, "next-input");

    auto& finalize = Block(function, finalize_id);
    const auto save_request = Project(builder, function, finalize, argument.id,
        CanonicalActionInputType(CanonicalAction::SavestateSaveImmutableArtifact),
        "save_request", scope);
    (void)Await(builder, function, finalize,
        CanonicalAction::SavestateSaveImmutableArtifact, save_request,
        "save-endpoint", scope);
    const auto neutral_tail = Constant(builder, function, finalize,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        FrameBytes(GCInputFrame{}), "tail-neutral", scope);
    const auto tail_begin_request = Construct(builder, function, finalize,
        CanonicalActionInputType(CanonicalAction::InputBeginDelivery),
        std::array{lease, neutral_tail}, "tail-begin-request", scope);
    const auto tail_binding = Await(builder, function, finalize,
        CanonicalAction::InputBeginDelivery, tail_begin_request,
        "tail-begin", scope);
    const auto before_tail_request = Construct(builder, function, finalize,
        CanonicalActionInputType(CanonicalAction::MovieObserveState), {},
        "observe-before-tail", scope);
    const auto before_tail = Await(builder, function, finalize,
        CanonicalAction::MovieObserveState, before_tail_request,
        "observe-before-tail", scope);
    const auto expected_count = Project(builder, function, finalize,
        before_tail, U64(), "current_input_count", scope);
    const auto advance_config = Constant(builder, function, finalize,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig),
        AdvanceConfig(), "tail-config", scope);
    const auto tail_request = Construct(builder, function, finalize,
        CanonicalActionInputType(
            CanonicalAction::ExecutionContinueUntilInputObserved),
        std::array{tail_binding, expected_count, advance_config},
        "tail-request", scope);
    (void)Await(builder, function, finalize,
        CanonicalAction::ExecutionContinueUntilInputObserved, tail_request,
        "tail-observed", scope);
    const auto tail_complete_request = Construct(builder, function, finalize,
        CanonicalActionInputType(CanonicalAction::InputCompleteDelivery),
        std::array{lease, tail_binding}, "tail-complete-request", scope);
    (void)Await(builder, function, finalize,
        CanonicalAction::InputCompleteDelivery, tail_complete_request,
        "tail-complete", scope);
    const auto after_tail_request = Construct(builder, function, finalize,
        CanonicalActionInputType(CanonicalAction::MovieObserveState), {},
        "observe-after-tail", scope);
    const auto after_tail = Await(builder, function, finalize,
        CanonicalAction::MovieObserveState, after_tail_request,
        "observe-after-tail", scope);
    const auto final_cursor = Project(builder, function, finalize, after_tail,
        U64(), "current_input_count", scope);
    (void)Await(builder, function, finalize,
        CanonicalAction::MovieStopRecording, recording, "finalize-dtm", scope);
    const auto result_epochs = Project(builder, function, finalize, argument.id,
        Named(EpochListSchema()), "epochs", scope);
    const auto result_source_count = Need(builder.AddInstruction(function,
        finalize, InstructionOpcode::ListSize, U64(),
        std::array{result_epochs}, {}, "source-count", std::nullopt, scope),
        "source count");
    const auto result_insert = Project(builder, function, finalize, argument.id,
        U64(), "insert_before", scope);
    const auto result_suffix = Binary(builder, function, finalize,
        InstructionOpcode::SubtractChecked, U64(), result_source_count,
        result_insert, "suffix", scope);
    const auto result_child = Binary(builder, function, finalize,
        InstructionOpcode::AddChecked, U64(), result_suffix,
        Constant(builder, function, finalize, U64(), std::uint64_t{1},
            "one", scope), "child-count", scope);
    const auto completed = EnumConstant(builder, function, finalize,
        OutcomeSchema(), 0, "completed", scope);
    const auto no_failure = EnumConstant(builder, function, finalize,
        FailureSchema(), 0, "no-failure", scope);
    const auto result_zero = Constant(builder, function, finalize, U64(),
        std::uint64_t{0}, "zero", scope);
    const auto result = Construct(builder, function, finalize,
        Named(RewriteResultSchema()), std::array{completed, result_insert,
            result_source_count, result_child, final_cursor, no_failure,
            result_zero, result_zero, result_zero}, "result", scope);
    (void)builder.AddInstruction(function, finalize,
        InstructionOpcode::ExitScope, std::nullopt, {}, {}, "release-scope",
        std::nullopt, scope);
    const auto succeeded = Constant(builder, function, finalize, Bool(), true,
        "succeeded");
    builder.SetTerminator(function, finalize, {.kind = TerminatorKind::Return,
        .return_value = result, .domain_outcome = succeeded}, "return");

    FailBlock(builder, function, Block(function, fail_prefix_id), scope,
        "tasmovie.input_epoch.prefix_diverged",
        "source playback cursor diverged before the insertion boundary");
    FailBlock(builder, function, Block(function, fail_movie_id), scope,
        "tasmovie.input_epoch.unexpected_movie_end",
        "source movie ended before the insertion boundary");

    module.accepted_policies = {.state_policies = {
            InvocationStatePolicy::EstablishBaseline},
        .execution_intents = {ExecutionIntent::Live},
        .permits_movie_playback = true, .permits_movie_recording = true};
    module.budgets = {.maximum_instructions = 24'000'000,
        .maximum_calls = 64, .maximum_call_depth = 8,
        .maximum_action_requests = 8'000'020, .maximum_emissions = 1,
        .maximum_artifacts = 2, .maximum_values = 24'000'000,
        .maximum_value_bytes = 512ull * 1024ull * 1024ull,
        .maximum_trace_events = 4'000'000};
    const auto sav = CanonicalActionArtifactPayloadSchemaIdentity(
        CanonicalAction::SavestateSaveImmutableArtifact);
    const auto dtm = CanonicalActionArtifactPayloadSchemaIdentity(
        CanonicalAction::MovieStopRecording);
    if (!sav || !dtm) throw std::logic_error("rewrite artifacts unavailable");
    builder.AddTypeImport(*sav);
    builder.AddTypeImport(*dtm);
    module.entrypoints = {{.name = std::string(RewriteEntrypoint),
        .function = function.id, .input_type = Named(RewriteRequestSchema()),
        .output_type = Named(RewriteResultSchema()),
        .domain_outcome_type = Bool(), .artifact_schemas = {*sav, *dtm},
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
        .executable_identity = std::string(
            capabilities::kSupportedExecutableIdentity),
        .backend = "jit64", .capability_packs = dependencies.capability_packs};
}

std::string ProfileHash(const RuntimeProfile& profile)
{
    const std::string value = profile.profile_id + '\0' + profile.game_id +
        '\0' + profile.disc_identity + '\0' + profile.executable_identity +
        '\0' + profile.backend;
    return hash::sha256(value.data(), value.size());
}

ProgramDependencyLock Verify(const ProgramModule& module)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const auto registered = capabilities::RegisterSourceCapabilityPacks(
        schemas, actions, packs);
    if (!registered.success) throw std::logic_error(registered.error.message);
    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success) throw std::logic_error(stored.error.message);
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const auto verified = verifier.Verify(stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        std::string message = "input-epoch module verification failed";
        for (const auto& item : verified.diagnostics)
            message += "; " + item.message;
        throw std::logic_error(message);
    }
    return verified.verified->dependency_lock;
}

class Graph
{
public:
    ProgramValueId Add(TypeRef type, ProgramValuePayload payload)
    {
        const auto id = ProgramValueId(next_++);
        values_.push_back({id, std::move(type), std::move(payload)});
        return id;
    }
    ProgramValueId Import(const ProgramValueGraph& source)
    {
        std::map<ProgramValueId, ProgramValueId> remap;
        for (const auto& value : source.values)
            remap.emplace(value.id, ProgramValueId(next_++));
        for (auto value : source.values)
        {
            value.id = remap.at(value.id);
            if (auto* record = std::get_if<RecordValue>(&value.payload))
                for (auto& field : record->fields) field = remap.at(field);
            if (auto* list = std::get_if<ListValue>(&value.payload))
                for (auto& item : list->elements) item = remap.at(item);
            if (auto* optional = std::get_if<program::OptionalValue>(&value.payload);
                optional && optional->value) optional->value = remap.at(*optional->value);
            values_.push_back(std::move(value));
        }
        return remap.at(source.root);
    }
    ProgramValueGraph Finish(ProgramValueId root)
    {
        return {root, std::move(values_)};
    }
private:
    std::uint64_t next_ = 1;
    std::vector<ProgramValue> values_;
};

ProgramValueGraph PrepareGraph(std::string_view path)
{
    CanonicalActionPayload payload;
    (void)payload.AddUtf8(CanonicalActionPayloadField::Path,
        std::string(path));
    const auto encoded = EncodeCanonicalActionPayload(payload,
        *CanonicalActionInputSchemaIdentity(
            CanonicalAction::MoviePrepareReadOnlyPlayback));
    return encoded.ok ? encoded.graph : ProgramValueGraph{};
}

ProgramValueGraph SaveGraph(std::string_view path)
{
    CanonicalActionPayload payload;
    (void)payload.AddUtf8(CanonicalActionPayloadField::Path,
        std::string(path));
    (void)payload.AddUtf8(CanonicalActionPayloadField::Label,
        "TAS movie input-epoch rewrite endpoint");
    const auto encoded = EncodeCanonicalActionPayload(payload,
        *CanonicalActionInputSchemaIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact));
    return encoded.ok ? encoded.graph : ProgramValueGraph{};
}

std::vector<Byte> RecordingConfig(std::string_view path)
{
    class Writer
    {
    public:
        void U32(std::uint32_t value) { for (unsigned s=0;s!=32;s+=8) b.push_back(static_cast<Byte>(value>>s)); }
        void Text(std::string_view value) { U32(static_cast<std::uint32_t>(value.size())); b.insert(b.end(),value.begin(),value.end()); }
        std::vector<Byte> b{'M','R','C','1'};
    } writer;
    writer.Text(path);
    writer.Text("TAS movie input-epoch rewrite");
    return std::move(writer.b);
}

ProgramValueGraph AnnotationInput(
    const TasMovieInputEpochAnnotationRequestV1& request)
{
    Graph graph;
    const auto prepare = graph.Import(PrepareGraph(request.source_dtm_path));
    const auto source_hash = graph.Add(Named(HashSchema()),
        request.source_dtm_sha256);
    const auto poll_count = graph.Add(U64(), request.source_poll_count);
    return graph.Finish(graph.Add(Named(AnnotationRequestSchema()),
        RecordValue{{prepare, source_hash, poll_count}}));
}

ProgramValueId AddEpoch(Graph& graph, const TasMovieInputEpochV1& epoch)
{
    const auto cursor = graph.Add(U64(), epoch.movie_input_cursor);
    const auto frame = graph.Add(CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload), FrameBytes(epoch.input));
    return graph.Add(Named(EpochSchema()),
        RecordValue{{cursor, frame}});
}

ProgramValueGraph RewriteInput(const TasMovieInputEpochRewriteRequestV1& request)
{
    Graph graph;
    const auto prepare = graph.Import(PrepareGraph(request.source_dtm_path));
    std::vector<ProgramValueId> epochs;
    epochs.reserve(request.schedule.epochs.size());
    for (const auto& epoch : request.schedule.epochs)
        epochs.push_back(AddEpoch(graph, epoch));
    const auto epoch_list = graph.Add(Named(EpochListSchema()),
        ListValue{std::move(epochs)});
    const auto insertion = graph.Add(U64(), request.insert_before_epoch);
    const auto recording = graph.Add(CanonicalRuntimeType(
        CanonicalRuntimeSchema::MovieRecordingStaticConfig),
        RecordingConfig(request.output_dtm_path));
    const auto save = graph.Import(SaveGraph(request.output_savestate_path));
    return graph.Finish(graph.Add(Named(RewriteRequestSchema()),
        RecordValue{{prepare, epoch_list, insertion, recording, save}}));
}

const ProgramValue* Find(const ProgramValueGraph& graph, ProgramValueId id)
{
    const auto found = std::ranges::find(graph.values, id, &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

template <typename T>
const T* Payload(const ProgramValueGraph& graph, ProgramValueId id)
{
    const auto* value = Find(graph, id);
    return value ? std::get_if<T>(&value->payload) : nullptr;
}

bool DecodeEpoch(const ProgramValueGraph& graph, ProgramValueId id,
    TasMovieInputEpochV1& epoch)
{
    const auto* record = Payload<RecordValue>(graph, id);
    if (!record || record->fields.size() != 2) return false;
    const auto* cursor = Payload<std::uint64_t>(graph, record->fields[0]);
    const auto* bytes = Payload<std::vector<Byte>>(graph, record->fields[1]);
    if (!cursor || !bytes || bytes->size() != 8) return false;
    epoch.movie_input_cursor = *cursor;
    epoch.input.buttons = static_cast<std::uint16_t>((*bytes)[0]) |
        (static_cast<std::uint16_t>((*bytes)[1]) << 8u);
    epoch.input.main_x = (*bytes)[2]; epoch.input.main_y = (*bytes)[3];
    epoch.input.c_x = (*bytes)[4]; epoch.input.c_y = (*bytes)[5];
    epoch.input.trig_l = (*bytes)[6]; epoch.input.trig_r = (*bytes)[7];
    return true;
}

template <typename Result>
bool DecodeCommonResult(std::span<const Byte> bytes,
    const ModuleIdentity& module, const ProgramDependencyLock& dependencies,
    std::string_view entrypoint, ProgramValueGraph const** output,
    std::vector<ProgramArtifact>* artifacts, std::string* diagnostic)
{
    const auto decoded = DecodeProgramResultV1(bytes);
    if (!decoded || !decoded.value || decoded.value->module != module ||
        decoded.value->entrypoint != entrypoint ||
        decoded.value->resolved_dependencies != dependencies ||
        decoded.value->infrastructure != ProgramInfrastructureStatus::Completed ||
        decoded.value->cleanup != ProgramCleanupStatus::Clean ||
        decoded.value->session_disposition != SessionDisposition::Clean ||
        !decoded.value->output || !decoded.value->domain_outcome)
    {
        Diagnostic(diagnostic, "input-epoch ProgramResult is not a clean exact result");
        return false;
    }
    const auto* domain = Payload<bool>(*decoded.value->domain_outcome,
        decoded.value->domain_outcome->root);
    if (!domain || !*domain) return false;
    *output = &*decoded.value->output;
    if (artifacts) *artifacts = decoded.value->artifacts;
    return true;
}

class AnnotationDefinition final : public IAnnotationFullPhaseDefinitionV1
{
public:
    AnnotationDefinition()
    {
        module_ = AnnotationModule(); dependencies_ = Verify(module_);
        const auto encoded = EncodeProgramModuleV1(module_);
        if (!encoded) throw std::logic_error(encoded.status.message);
        envelope_ = {{module_.identity.canonical_id, module_.identity.revision,
            module_.identity.module_hash.ToHex()}, kProgramCodecVersionV1,
            false, encoded.bytes};
        profile_ = Profile(dependencies_);
        runtime_ = {.module = envelope_.identity,
            .entrypoint = std::string(AnnotationEntrypoint),
            .dependency_lock_sha256 = ComputeProgramDependencyLockHashV1(
                dependencies_).ToHex(),
            .runtime_profile_sha256 = ProfileHash(profile_),
            .state_policy = InvocationStatePolicy::EstablishBaseline,
            .execution = {.intent = ExecutionIntent::Live,
                .allow_movie_playback = true}, .limits = module_.budgets,
            .baseline_lineage = std::string(AnnotationBaselineLineage)};
        const TasMovieInputEpochAnnotationRequestV1 sample{
            .source_dtm_path = "source.dtm",
            .source_dtm_sha256 = std::string(64, '0'),
            .source_poll_count = 1};
        const auto invocation = Resolve(sample, ProgramExecutionId(1),
            AttemptId(1));
        runtime_.verified_dependency_sha256 =
            ComputeProgramInvocationCompatibilityHashV1(invocation);
        const std::string movie = "tasmovie.input_epoch/annotate-read-only/v1";
        const std::string service = "tasmovie.input_epoch/padread-cursor/v1";
        runtime_.movie_policy_sha256 = hash::sha256(movie.data(), movie.size());
        runtime_.service_policy_sha256 = hash::sha256(service.data(), service.size());
        const std::string canonical = runtime_.module.canonical_hash +
            runtime_.verified_dependency_sha256 + runtime_.service_policy_sha256;
        identity_ = {static_cast<std::int32_t>(
                savor::PK_TasMovieAnnotateInputEpochs), ProgramVersion,
            std::string(AnnotationFullPhaseCanonicalId), 1,
            hash::sha256(canonical.data(), canonical.size())};
    }
    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override { return runtime_; }
    const EncodedModuleEnvelope& module_envelope() const noexcept override { return envelope_; }
    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> bytes, ProgramExecutionId execution,
        AttemptId attempt, std::string* diagnostic) const override
    {
        TasMovieInputEpochAnnotationRequestV1 request;
        if (!DecodeAnnotationExecutionInputV1(bytes, request, diagnostic)) return {};
        return Resolve(request, execution, attempt);
    }
    bool DecodeProgramResult(std::span<const Byte> bytes,
        TasMovieInputEpochAnnotationResultV1& result,
        std::string* diagnostic) const override
    {
        const ProgramValueGraph* graph = nullptr;
        if (!DecodeCommonResult<TasMovieInputEpochAnnotationResultV1>(bytes,
                module_.identity, dependencies_, AnnotationEntrypoint, &graph,
                nullptr, diagnostic)) return false;
        const auto* record = Payload<RecordValue>(*graph, graph->root);
        if (!record || record->fields.size() != 8) return false;
        const auto* outcome = Payload<EnumValue>(*graph, record->fields[0]);
        const auto* source_hash = Payload<std::string>(*graph, record->fields[1]);
        const auto* source_count = Payload<std::uint64_t>(*graph, record->fields[2]);
        const auto* epochs = Payload<ListValue>(*graph, record->fields[3]);
        const auto* failure = Payload<EnumValue>(*graph, record->fields[4]);
        const auto* failure_epoch = Payload<std::uint64_t>(*graph, record->fields[5]);
        const auto* expected = Payload<std::uint64_t>(*graph, record->fields[6]);
        const auto* actual = Payload<std::uint64_t>(*graph, record->fields[7]);
        if (!outcome || !source_hash || !source_count || !epochs || !failure ||
            !failure_epoch || !expected || !actual) return false;
        result = {};
        result.outcome = static_cast<InputEpochOutcomeV1>(outcome->value);
        result.schedule.source_dtm_sha256 = *source_hash;
        result.schedule.source_poll_count = *source_count;
        result.schedule.epochs.resize(epochs->elements.size());
        for (std::size_t i=0;i!=epochs->elements.size();++i)
            if (!DecodeEpoch(*graph, epochs->elements[i], result.schedule.epochs[i])) return false;
        result.failure_reason = static_cast<InputEpochFailureReasonV1>(failure->value);
        result.failure_epoch = *failure_epoch;
        result.expected_cursor = *expected;
        result.actual_cursor = *actual;
        return result.outcome == InputEpochOutcomeV1::Completed &&
            ValidateInputEpochScheduleV1(result.schedule, diagnostic);
    }
private:
    ProgramInvocation Resolve(const TasMovieInputEpochAnnotationRequestV1& request,
        ProgramExecutionId execution, AttemptId attempt) const
    {
        return {.invocation_id = execution, .attempt_id = attempt,
            .module = module_.identity, .entrypoint = std::string(AnnotationEntrypoint),
            .dependencies = dependencies_, .runtime_profile = profile_,
            .state = {.policy = InvocationStatePolicy::EstablishBaseline,
                .session_lineage = std::string(AnnotationBaselineLineage)},
            .execution = runtime_.execution, .input = AnnotationInput(request),
            .limits = module_.budgets,
            .provenance = {.requesting_component = "SavorDb.tasmovie.input_epoch.annotation"}};
    }
    ProgramModule module_; ProgramDependencyLock dependencies_; RuntimeProfile profile_;
    EncodedModuleEnvelope envelope_; fullphase::FullPhaseRuntimeContract runtime_;
    fullphase::FullPhaseProgramIdentity identity_;
};

class RewriteDefinition final : public IRewriteFullPhaseDefinitionV1
{
public:
    RewriteDefinition()
    {
        module_ = RewriteModule(); dependencies_ = Verify(module_);
        const auto encoded = EncodeProgramModuleV1(module_);
        if (!encoded) throw std::logic_error(encoded.status.message);
        envelope_ = {{module_.identity.canonical_id, module_.identity.revision,
            module_.identity.module_hash.ToHex()}, kProgramCodecVersionV1,
            false, encoded.bytes};
        profile_ = Profile(dependencies_);
        runtime_ = {.module = envelope_.identity,
            .entrypoint = std::string(RewriteEntrypoint),
            .dependency_lock_sha256 = ComputeProgramDependencyLockHashV1(
                dependencies_).ToHex(), .runtime_profile_sha256 = ProfileHash(profile_),
            .state_policy = InvocationStatePolicy::EstablishBaseline,
            .execution = {.intent = ExecutionIntent::Live,
                .allow_movie_playback = true, .allow_movie_recording = true,
                .allow_input = true}, .limits = module_.budgets,
            .baseline_lineage = std::string(RewriteBaselineLineage)};
        TasMovieInputEpochScheduleV1 schedule{.source_dtm_sha256 = std::string(64,'0'),
            .source_poll_count = 1, .epochs = {{.movie_input_cursor = 1}}};
        const TasMovieInputEpochRewriteRequestV1 sample{.source_dtm_path="source.dtm",
            .schedule=std::move(schedule), .insert_before_epoch=0,
            .output_dtm_path="child.dtm", .output_savestate_path="child.sav"};
        const auto invocation = Resolve(sample, ProgramExecutionId(1), AttemptId(1));
        runtime_.verified_dependency_sha256 = ComputeProgramInvocationCompatibilityHashV1(invocation);
        const std::string movie = "tasmovie.input_epoch/playback-branch-record/v1";
        const std::string service = "tasmovie.input_epoch/held-guest-acknowledged/v1";
        runtime_.movie_policy_sha256 = hash::sha256(movie.data(),movie.size());
        runtime_.service_policy_sha256 = hash::sha256(service.data(),service.size());
        const std::string canonical = runtime_.module.canonical_hash +
            runtime_.verified_dependency_sha256 + runtime_.service_policy_sha256;
        identity_ = {static_cast<std::int32_t>(savor::PK_TasMovieRewriteInputEpochs),
            ProgramVersion, std::string(RewriteFullPhaseCanonicalId), 1,
            hash::sha256(canonical.data(),canonical.size())};
    }
    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override { return runtime_; }
    const EncodedModuleEnvelope& module_envelope() const noexcept override { return envelope_; }
    std::optional<ProgramInvocation> BuildResolvedExecution(std::span<const std::uint8_t> bytes,
        ProgramExecutionId execution, AttemptId attempt, std::string* diagnostic) const override
    {
        TasMovieInputEpochRewriteRequestV1 request;
        if (!DecodeRewriteExecutionInputV1(bytes, request, diagnostic)) return {};
        return Resolve(request, execution, attempt);
    }
    bool DecodeProgramResult(std::span<const Byte> bytes,
        TasMovieInputEpochRewriteResultV1& result, std::string* diagnostic) const override
    {
        const ProgramValueGraph* graph = nullptr;
        std::vector<ProgramArtifact> artifacts;
        if (!DecodeCommonResult<TasMovieInputEpochRewriteResultV1>(bytes,
                module_.identity, dependencies_, RewriteEntrypoint, &graph,
                &artifacts, diagnostic)) return false;
        const auto* record = Payload<RecordValue>(*graph, graph->root);
        if (!record || record->fields.size()!=9) return false;
        const auto* outcome=Payload<EnumValue>(*graph,record->fields[0]);
        const auto* insert=Payload<std::uint64_t>(*graph,record->fields[1]);
        const auto* source=Payload<std::uint64_t>(*graph,record->fields[2]);
        const auto* child=Payload<std::uint64_t>(*graph,record->fields[3]);
        const auto* cursor=Payload<std::uint64_t>(*graph,record->fields[4]);
        const auto* failure=Payload<EnumValue>(*graph,record->fields[5]);
        const auto* failure_epoch=Payload<std::uint64_t>(*graph,record->fields[6]);
        const auto* expected=Payload<std::uint64_t>(*graph,record->fields[7]);
        const auto* actual=Payload<std::uint64_t>(*graph,record->fields[8]);
        if(!outcome||!insert||!source||!child||!cursor||!failure||!failure_epoch||!expected||!actual||artifacts.size()!=2)return false;
        result={.outcome=static_cast<InputEpochOutcomeV1>(outcome->value),
            .insert_before_epoch=*insert,.source_epoch_count=*source,
            .child_epoch_count=*child,.final_cursor=*cursor,
            .failure_reason=static_cast<InputEpochFailureReasonV1>(failure->value),
            .failure_epoch=*failure_epoch,.expected_cursor=*expected,
            .actual_cursor=*actual,.artifacts=std::move(artifacts)};
        return result.outcome==InputEpochOutcomeV1::Completed &&
            result.child_epoch_count==result.source_epoch_count-result.insert_before_epoch+1;
    }
private:
    ProgramInvocation Resolve(const TasMovieInputEpochRewriteRequestV1& request,
        ProgramExecutionId execution, AttemptId attempt) const
    {
        return {.invocation_id=execution,.attempt_id=attempt,.module=module_.identity,
            .entrypoint=std::string(RewriteEntrypoint),.dependencies=dependencies_,
            .runtime_profile=profile_,.state={.policy=InvocationStatePolicy::EstablishBaseline,
                .session_lineage=std::string(RewriteBaselineLineage)},
            .execution=runtime_.execution,.input=RewriteInput(request),
            .limits=module_.budgets,
            .provenance={.requesting_component="SavorDb.tasmovie.input_epoch.rewrite"}};
    }
    ProgramModule module_; ProgramDependencyLock dependencies_; RuntimeProfile profile_;
    EncodedModuleEnvelope envelope_; fullphase::FullPhaseRuntimeContract runtime_;
    fullphase::FullPhaseProgramIdentity identity_;
};

} // namespace

std::shared_ptr<const IAnnotationFullPhaseDefinitionV1>
AnnotationFullPhaseDefinitionV1()
{
    static const auto value = std::make_shared<const AnnotationDefinition>();
    return value;
}

std::shared_ptr<const IRewriteFullPhaseDefinitionV1>
RewriteFullPhaseDefinitionV1()
{
    static const auto value = std::make_shared<const RewriteDefinition>();
    return value;
}

} // namespace savor::runtime::tasmovie::inputepoch
