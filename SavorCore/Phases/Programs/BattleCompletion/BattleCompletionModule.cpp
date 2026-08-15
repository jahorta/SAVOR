#include "BattleCompletionModule.h"

#include "Runner/Runtime/ProgramKind.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Composition/BattleCompletionComposition.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
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

namespace savor::runtime::battlecompletion {
namespace {

using namespace program;
using namespace program::composition;
using Builder = program::composition::detail::ModuleFragmentBuilder;

constexpr std::array<std::uint8_t, 4> kWireMagic{'B','C','P','1'};
constexpr std::uint32_t kVictoryPc = 0x800706d8u;

void SetDiagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

SchemaIdentity ExactSchema(std::string id, std::string contract)
{
    SchemaIdentity schema{.canonical_id = std::move(id), .version = 1};
    schema.schema_hash = ContentHash256::FromHex(
        hash::sha256(contract.data(), contract.size())).value();
    return schema;
}

SchemaIdentity RequestSchema()
{
    return ExactSchema(
        "soa.battle.completion.Request",
        "record BattleCompletionRequest/1(battle_set_id:u64,wave_id:u64,turn_job_id:u64,execution_job_id:u64,save_request:runtime.action.savestate_save_immutable_artifact.Input/1)");
}

SchemaIdentity ResultSchema()
{
    return ExactSchema(
        "soa.battle.completion.Result",
        "record BattleCompletionResult/1(manifest:soa.battle.completion.Manifest/1,transition:soa.field.TransitionContext/1)");
}

TypeRef RequestType() { return TypeRef::Named(RequestSchema()); }
TypeRef ResultType() { return TypeRef::Named(ResultSchema()); }

InstructionTarget Action(CanonicalAction action)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = CanonicalActionIdentity(action),
    };
}

ProgramValueId Need(std::optional<ProgramValueId> value, std::string_view what)
{
    if (!value)
        throw std::logic_error("battle.completion lowering failed: " +
            std::string(what));
    return *value;
}

BasicBlock& Block(ProgramFunction& function, ProgramBlockId id)
{
    const auto found = std::ranges::find(function.blocks, id, &BasicBlock::id);
    if (found == function.blocks.end())
        throw std::logic_error("battle.completion block is unavailable");
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
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    TypeRef type, LiteralPayload payload, std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::Constant, type, {}, {},
        std::move(selector), LiteralValue{type, std::move(payload)}),
        "constant");
}

ProgramValueId Construct(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    TypeRef type, std::span<const ProgramValueId> fields,
    std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct, type, fields,
        {}, std::move(selector)), "record");
}

ProgramValueId Project(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId value, TypeRef type, std::string field)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordProject, type,
        std::array{value}, {}, std::move(field)), "projection");
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
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(CanonicalAction::ExecutionRequirePausedPc),
        std::array{request}, Action(CanonicalAction::ExecutionRequirePausedPc),
        selector + "/require"), "paused-PC check");
}

ProgramModule ConstructModule()
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::string(ModuleCanonicalId),
            .revision = 1,
        }};
    const auto completion = LowerBattleCompletionSequenceV1(module);
    if (!completion || !completion.function)
        throw std::logic_error("Battle completion sequence lowering failed");

    Builder builder(module, "BattleCompletionModule", "battle.completion/complete/v1");
    builder.AddLocalType({
        .identity = RequestSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"battle_set_id", TypeRef::Builtin(BuiltinType::U64)},
            {"wave_id", TypeRef::Builtin(BuiltinType::U64)},
            {"turn_job_id", TypeRef::Builtin(BuiltinType::U64)},
            {"execution_job_id", TypeRef::Builtin(BuiltinType::U64)},
            {"save_request", CanonicalActionInputType(
                CanonicalAction::SavestateSaveImmutableArtifact)},
        }});
    builder.AddLocalType({
        .identity = ResultSchema(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"manifest", TypeRef::Named(
                capabilities::BattleCompletionManifestSchemaIdentity())},
            {"transition", TypeRef::Named(
                capabilities::FieldTransitionContextSchemaIdentity())},
        }});
    for (const auto action : {
             CanonicalAction::ExecutionRequirePausedPc,
             CanonicalAction::SavestateSaveImmutableArtifact})
        AddAction(builder, action);

    const auto request_argument = builder.NewArgument(RequestType());
    auto& function = builder.AddFunction(
        std::string(Entrypoint), std::array{request_argument}, ResultType(),
        TypeRef::Builtin(BuiltinType::Bool), true);
    const auto entry_id = builder.AddBlock(function).id;
    auto& entry = Block(function, entry_id);
    const auto entry_receipt = RequirePc(
        builder, function, entry, kVictoryPc, "entry");
    const auto entry_pc = Project(
        builder, function, entry, entry_receipt,
        TypeRef::Builtin(BuiltinType::U32), "pc");
    const auto entry_vi = Project(
        builder, function, entry, entry_receipt,
        TypeRef::Builtin(BuiltinType::U64), "vi_count");
    const auto entry_epoch = Project(
        builder, function, entry, entry_receipt,
        TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
    const auto neutral = Constant(
        builder, function, entry,
        CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
        std::vector<Byte>{0,0,128,128,128,128,0,0},
        "completion/neutral-input");
    const auto battle_set_id = Project(
        builder, function, entry, request_argument.id,
        TypeRef::Builtin(BuiltinType::U64), "battle_set_id");
    const auto wave_id = Project(
        builder, function, entry, request_argument.id,
        TypeRef::Builtin(BuiltinType::U64), "wave_id");
    const auto turn_job_id = Project(
        builder, function, entry, request_argument.id,
        TypeRef::Builtin(BuiltinType::U64), "turn_job_id");
    const auto execution_job_id = Project(
        builder, function, entry, request_argument.id,
        TypeRef::Builtin(BuiltinType::U64), "execution_job_id");
    const auto sequence = Need(builder.AddInstruction(
        function, entry, InstructionOpcode::CallLocal,
        TypeRef::Named(BattleCompletionSequenceReceiptSchemaIdentity()),
        std::array{entry_pc, entry_vi, entry_epoch, neutral,
                   battle_set_id, wave_id, turn_job_id, execution_job_id},
        {
            .kind = InstructionTargetKind::LocalFunction,
            .local_function = *completion.function,
        },
        "completion/sequence"), "completion sequence");
    const auto transition = Need(builder.AddInstruction(
        function, entry, InstructionOpcode::RecordProject,
        TypeRef::Named(capabilities::FieldTransitionContextSchemaIdentity()),
        std::array{sequence}, {}, "transition"), "sequence transition");
    const auto manifest = Need(builder.AddInstruction(
        function, entry, InstructionOpcode::RecordProject,
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        std::array{sequence}, {}, "manifest"), "sequence manifest");
    const auto save_request = Project(
        builder, function, entry, request_argument.id,
        CanonicalActionInputType(CanonicalAction::SavestateSaveImmutableArtifact),
        "save_request");
    (void)Need(builder.AddInstruction(
        function, entry, InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(CanonicalAction::SavestateSaveImmutableArtifact),
        std::array{save_request},
        Action(CanonicalAction::SavestateSaveImmutableArtifact),
        "completion/save-preseed-successor"), "completion successor");
    const auto result = Construct(
        builder, function, entry, ResultType(),
        std::array{manifest, transition}, "completion/result");
    const auto succeeded = Constant(
        builder, function, entry, TypeRef::Builtin(BuiltinType::Bool),
        true, "completion/succeeded");
    builder.SetTerminator(function, entry, {
        .kind = TerminatorKind::Return,
        .return_value = result,
        .domain_outcome = succeeded,
    }, "completion/return");

    module.accepted_policies = {
        .state_policies = {InvocationStatePolicy::RestoreBaseline},
        .execution_intents = {ExecutionIntent::Live},
    };
    module.budgets = {
        .maximum_instructions = 16'384,
        .maximum_calls = 256,
        .maximum_call_depth = 8,
        .maximum_action_requests = 1'024,
        .maximum_emissions = 4,
        .maximum_artifacts = 1,
        .maximum_values = 32'768,
        .maximum_value_bytes = 32 * 1024 * 1024,
        .maximum_trace_events = 16'384,
    };
    const auto savestate_artifact = CanonicalActionArtifactPayloadSchemaIdentity(
        CanonicalAction::SavestateSaveImmutableArtifact);
    if (!savestate_artifact)
        throw std::logic_error("savestate artifact schema is unavailable");
    builder.AddTypeImport(*savestate_artifact);
    module.entrypoints = {{
        .name = std::string(Entrypoint),
        .function = function.id,
        .input_type = RequestType(),
        .output_type = ResultType(),
        .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
        .artifact_schemas = {*savestate_artifact},
        .required_capability_packs = module.required_capability_packs,
        .accepted_policies = module.accepted_policies,
    }};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

RuntimeProfile Profile(const ProgramDependencyLock& dependencies)
{
    return {
        .profile_id = "soa-usa-jit64-v1",
        .game_id = std::string(capabilities::kSupportedGameId),
        .disc_identity = std::string(capabilities::kSupportedGameId),
        .executable_identity = std::string(
            capabilities::kSupportedExecutableIdentity),
        .backend = "jit64",
        .capability_packs = dependencies.capability_packs,
    };
}

std::string ProfileHash(const RuntimeProfile& profile)
{
    std::string canonical = profile.profile_id + '\0' + profile.game_id +
        '\0' + profile.disc_identity + '\0' + profile.executable_identity +
        '\0' + profile.backend;
    return hash::sha256(canonical.data(), canonical.size());
}

std::optional<ProgramDependencyLock> Verify(
    const ProgramModule& module, std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const auto source = capabilities::RegisterSourceCapabilityPacks(
        schemas, actions, packs);
    if (!source.success)
    {
        SetDiagnostic(diagnostic, source.error.message);
        return std::nullopt;
    }
    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success)
    {
        SetDiagnostic(diagnostic, stored.error.message);
        return std::nullopt;
    }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const auto verified = verifier.Verify(
        stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        std::string message = "battle.completion verification failed";
        for (const auto& item : verified.diagnostics)
            message += "; " + item.message;
        SetDiagnostic(diagnostic, std::move(message));
        return std::nullopt;
    }
    return verified.verified->dependency_lock;
}

class GraphAssembler
{
public:
    ProgramValueId Add(TypeRef type, ProgramValuePayload payload)
    {
        const auto id = ProgramValueId(next_++);
        values_.push_back({id, std::move(type), std::move(payload)});
        return id;
    }
    ProgramValueId Import(const ProgramValueGraph& graph)
    {
        std::map<ProgramValueId, ProgramValueId> remap;
        for (const auto& value : graph.values)
            remap.emplace(value.id, ProgramValueId(next_++));
        for (const auto& value : graph.values)
        {
            auto copy = value;
            copy.id = remap.at(value.id);
            if (auto* record = std::get_if<RecordValue>(&copy.payload))
                for (auto& field : record->fields) field = remap.at(field);
            if (auto* list = std::get_if<ListValue>(&copy.payload))
                for (auto& item : list->elements) item = remap.at(item);
            if (auto* optional = std::get_if<OptionalValue>(&copy.payload);
                optional && optional->value)
                optional->value = remap.at(*optional->value);
            values_.push_back(std::move(copy));
        }
        return remap.at(graph.root);
    }
    ProgramValueGraph Finish(ProgramValueId root)
    {
        return {root, std::move(values_)};
    }
private:
    std::uint64_t next_ = 1;
    std::vector<ProgramValue> values_;
};

ProgramValueGraph InputGraph(
    const BattleCompletionRequestV1& request,
    std::string* diagnostic)
{
    GraphAssembler graph;
    const auto battle_set = graph.Add(
        TypeRef::Builtin(BuiltinType::U64), request.lineage.battle_set_id);
    const auto wave = graph.Add(
        TypeRef::Builtin(BuiltinType::U64), request.lineage.wave_id);
    const auto turn_job = graph.Add(
        TypeRef::Builtin(BuiltinType::U64), request.lineage.turn_job_id);
    const auto execution_job = graph.Add(
        TypeRef::Builtin(BuiltinType::U64), request.lineage.execution_job_id);
    CanonicalActionPayload payload;
    if (!payload.AddUtf8(
            CanonicalActionPayloadField::Path,
            request.output_savestate_path) ||
        !payload.AddUtf8(
            CanonicalActionPayloadField::Label,
            "battle.completion accepted field preseed"))
    {
        SetDiagnostic(diagnostic, "completion savestate request is invalid");
        return {};
    }
    const auto encoded = EncodeCanonicalActionPayload(
        payload,
        *CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact).named);
    if (!encoded.ok)
    {
        SetDiagnostic(diagnostic, encoded.diagnostic);
        return {};
    }
    const auto save = graph.Import(encoded.graph);
    const auto root = graph.Add(
        RequestType(), RecordValue{{battle_set, wave, turn_job,
                                    execution_job, save}});
    return graph.Finish(root);
}

const ProgramValue* Find(
    const ProgramValueGraph& graph, ProgramValueId id)
{
    const auto found = std::ranges::find(graph.values, id, &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
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

bool DecodeOutput(
    const ProgramValueGraph& graph,
    BattleCompletionResultV1& output,
    std::string* diagnostic)
{
    const auto* root = Find(graph, graph.root);
    const auto* record = root && root->type == ResultType()
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    if (!record || record->fields.size() != 2)
    {
        SetDiagnostic(diagnostic, "battle.completion result record is malformed");
        return false;
    }
    const auto decode_bytes = [&](std::size_t index,
                                  const SchemaIdentity& schema)
        -> const std::vector<Byte>*
    {
        const auto* value = Find(graph, record->fields[index]);
        return value && value->type == TypeRef::Named(schema)
            ? std::get_if<std::vector<Byte>>(&value->payload)
            : nullptr;
    };
    const auto* manifest = decode_bytes(
        0, capabilities::BattleCompletionManifestSchemaIdentity());
    const auto* transition = decode_bytes(
        1, capabilities::FieldTransitionContextSchemaIdentity());
    if (!manifest || !transition ||
        !DecodeBattleCompletionManifestV1(*manifest, output.manifest) ||
        !DecodeFieldTransitionContextV1(*transition, output.transition))
    {
        SetDiagnostic(diagnostic, "battle.completion typed output is malformed");
        return false;
    }
    return true;
}

class Definition final : public IBattleCompletionFullPhaseDefinitionV1
{
public:
    Definition()
    {
        const auto module = ConstructModule();
        std::string diagnostic;
        const auto dependencies = Verify(module, &diagnostic);
        const auto encoded = EncodeProgramModuleV1(module);
        if (!dependencies || !encoded)
            throw std::logic_error(diagnostic.empty()
                ? encoded.status.message : diagnostic);
        dependencies_ = *dependencies;
        module_identity_ = module.identity;
        profile_ = Profile(dependencies_);
        envelope_ = {
            .identity = {module.identity.canonical_id, module.identity.revision,
                         module.identity.module_hash.ToHex()},
            .format_version = kProgramCodecVersionV1,
            .development_only = false,
            .payload = encoded.bytes,
        };
        const InvocationExecutionPolicy execution{
            .intent = ExecutionIntent::Live,
            .allow_input = true,
            .record_trace = false,
        };
        const BattleCompletionRequestV1 sample{
            .lineage = {1,1,1,1},
            .output_savestate_path = "battle-completion.sav",
        };
        constexpr std::string_view movie_policy =
            "soa.battle.completion/no-movie/v1";
        constexpr std::string_view service_policy =
            "soa.battle.completion/adaptive-rewards-preseed/v1";
        runtime_ = {
            .module = envelope_.identity,
            .entrypoint = std::string(Entrypoint),
            .dependency_lock_sha256 =
                ComputeProgramDependencyLockHashV1(dependencies_).ToHex(),
            .runtime_profile_sha256 = ProfileHash(profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = execution,
            .limits = module.budgets,
            .baseline_lineage = std::string(BaselineLineage),
            .movie_policy_sha256 = hash::sha256(
                movie_policy.data(), movie_policy.size()),
            .service_policy_sha256 = hash::sha256(
                service_policy.data(), service_policy.size()),
        };
        const auto invocation = Resolve(
            sample, ProgramExecutionId(1), AttemptId(1), &diagnostic);
        if (!invocation)
            throw std::logic_error("battle.completion sample input is invalid");
        runtime_.verified_dependency_sha256 =
            ComputeProgramInvocationCompatibilityHashV1(*invocation);
        std::string canonical = std::string(FullPhaseCanonicalId) + '\0' +
            runtime_.module.canonical_hash + '\0' +
            runtime_.verified_dependency_sha256;
        identity_ = {
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletion),
            .program_version = ProgramVersion,
            .canonical_id = std::string(FullPhaseCanonicalId),
            .contract_revision = 1,
            .canonical_sha256 = hash::sha256(canonical.data(), canonical.size()),
        };
    }

    const fullphase::FullPhaseProgramIdentity& identity() const noexcept override
    { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract() const noexcept override
    { return runtime_; }
    const EncodedModuleEnvelope& module_envelope() const noexcept override
    { return envelope_; }
    fullphase::FullPhaseWorksetPolicy workset_policy() const noexcept override
    { return {.minimum_item_count = 1, .maximum_item_count = 1}; }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> input,
        ProgramExecutionId execution,
        AttemptId attempt,
        std::string* diagnostic) const override
    {
        BattleCompletionRequestV1 request;
        if (!DecodeBattleCompletionExecutionInputV1(
                input, request, diagnostic))
            return std::nullopt;
        return Resolve(request, execution, attempt, diagnostic);
    }

    bool DecodeProgramResult(
        std::span<const Byte> bytes,
        BattleCompletionResultV1& output,
        std::string* diagnostic) const override
    {
        const auto decoded = DecodeProgramResultV1(bytes);
        if (!decoded || !decoded.value ||
            decoded.value->module != module_identity_ ||
            decoded.value->entrypoint != Entrypoint ||
            decoded.value->resolved_dependencies != dependencies_ ||
            decoded.value->infrastructure != ProgramInfrastructureStatus::Completed ||
            decoded.value->cleanup != ProgramCleanupStatus::Clean ||
            decoded.value->session_disposition != SessionDisposition::Clean ||
            !decoded.value->output || !decoded.value->domain_outcome ||
            !DomainOutcomeIsTrue(*decoded.value->domain_outcome))
        {
            SetDiagnostic(diagnostic, "battle.completion did not complete cleanly");
            return false;
        }
        if (!DecodeOutput(*decoded.value->output, output, diagnostic))
            return false;
        output.artifacts = decoded.value->artifacts;
        return true;
    }

private:
    std::optional<ProgramInvocation> Resolve(
        const BattleCompletionRequestV1& request,
        ProgramExecutionId execution,
        AttemptId attempt,
        std::string* diagnostic) const
    {
        if (!execution || !attempt ||
            request.lineage.battle_set_id == 0 ||
            request.lineage.wave_id == 0 ||
            request.lineage.turn_job_id == 0 ||
            request.lineage.execution_job_id == 0 ||
            request.output_savestate_path.empty())
        {
            SetDiagnostic(diagnostic, "battle.completion request is incomplete");
            return std::nullopt;
        }
        auto input = InputGraph(request, diagnostic);
        if (!input.root) return std::nullopt;
        return ProgramInvocation{
            .invocation_id = execution,
            .attempt_id = attempt,
            .module = module_identity_,
            .entrypoint = std::string(Entrypoint),
            .dependencies = dependencies_,
            .runtime_profile = profile_,
            .state = {
                .policy = InvocationStatePolicy::RestoreBaseline,
                .session_lineage = std::string(BaselineLineage),
            },
            .execution = runtime_.execution,
            .input = std::move(input),
            .limits = runtime_.limits,
            .provenance = {
                .requesting_component = "SavorDb.battle.completion",
                .attributes = {{"contract", "soa.battle.completion/complete@1"}},
            },
        };
    }

    fullphase::FullPhaseProgramIdentity identity_;
    fullphase::FullPhaseRuntimeContract runtime_;
    EncodedModuleEnvelope envelope_;
    ModuleIdentity module_identity_;
    ProgramDependencyLock dependencies_;
    RuntimeProfile profile_;
};

class Writer
{
public:
    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            U8(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value)
    {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32u));
    }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool U8(std::uint8_t& value)
    {
        if (offset_ == bytes_.size()) return false;
        value = bytes_[offset_++]; return true;
    }
    bool U32(std::uint32_t& value)
    {
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            std::uint8_t byte = 0;
            if (!U8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        std::uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        value = static_cast<std::uint64_t>(lo) |
            (static_cast<std::uint64_t>(hi) << 32u);
        return true;
    }
    bool Text(std::string& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > 32768 || size > bytes_.size() - offset_)
            return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size; return true;
    }
    bool Done() const { return offset_ == bytes_.size(); }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

} // namespace

std::vector<std::uint8_t> EncodeBattleCompletionExecutionInputV1(
    const BattleCompletionRequestV1& request)
{
    Writer writer;
    for (const auto byte : kWireMagic) writer.U8(byte);
    writer.U64(request.lineage.battle_set_id);
    writer.U64(request.lineage.wave_id);
    writer.U64(request.lineage.turn_job_id);
    writer.U64(request.lineage.execution_job_id);
    writer.Text(request.output_savestate_path);
    return std::move(writer.bytes);
}

bool DecodeBattleCompletionExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionRequestV1& request,
    std::string* diagnostic)
{
    Reader reader(bytes);
    for (const auto expected : kWireMagic)
    {
        std::uint8_t actual = 0;
        if (!reader.U8(actual) || actual != expected)
        {
            SetDiagnostic(diagnostic, "battle.completion input magic is invalid");
            return false;
        }
    }
    BattleCompletionRequestV1 decoded;
    if (!reader.U64(decoded.lineage.battle_set_id) ||
        !reader.U64(decoded.lineage.wave_id) ||
        !reader.U64(decoded.lineage.turn_job_id) ||
        !reader.U64(decoded.lineage.execution_job_id) ||
        !reader.Text(decoded.output_savestate_path) || !reader.Done())
    {
        SetDiagnostic(diagnostic, "battle.completion input is malformed");
        return false;
    }
    request = std::move(decoded);
    SetDiagnostic(diagnostic, {});
    return true;
}

std::shared_ptr<const IBattleCompletionFullPhaseDefinitionV1>
BattleCompletionFullPhaseDefinitionV1()
{
    static const auto definition = std::make_shared<const Definition>();
    return definition;
}

} // namespace savor::runtime::battlecompletion
