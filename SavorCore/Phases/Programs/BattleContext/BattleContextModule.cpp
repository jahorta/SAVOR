#include "BattleContextModule.h"

#include "Runner/Runtime/ProgramKind.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
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
#include <string_view>
#include <utility>

namespace savor::runtime::battlecontext {
namespace {

using namespace program;
using namespace program::composition;

void SetDiagnostic(std::string* output, std::string value)
{
    if (output != nullptr)
        *output = std::move(value);
}

std::string TypeContract(const TypeRef& type)
{
    if (!type.is_named())
    {
        return "builtin:" + std::to_string(
            static_cast<std::uint8_t>(type.builtin));
    }
    return type.named->canonical_id + "/" +
        std::to_string(type.named->version) + "#" +
        type.named->schema_hash.ToHex();
}

TypeRef CaptureResultType()
{
    return TypeRef::Named(
        BattleContextCaptureResultSchemaIdentityV1());
}

RuntimeProfile InvocationRuntimeProfile(
    const ProgramDependencyLock& dependencies)
{
    return {
        .profile_id = "soa-usa-jit64-v1",
        .game_id = std::string(
            capabilities::kSupportedGameId),
        .disc_identity = std::string(
            capabilities::kSupportedGameId),
        .executable_identity = std::string(
            capabilities::kSupportedExecutableIdentity),
        .backend = "jit64",
        .capability_packs = dependencies.capability_packs,
    };
}

std::string RuntimeProfileHash(const RuntimeProfile& profile)
{
    std::string canonical = profile.profile_id;
    for (std::string_view value : {
             std::string_view(profile.game_id),
             std::string_view(profile.disc_identity),
             std::string_view(profile.executable_identity),
             std::string_view(profile.backend)})
    {
        canonical.push_back('\0');
        canonical.append(value);
    }
    return hash::sha256(canonical.data(), canonical.size());
}

SemanticObservationComposition BattleContextObservation()
{
    const auto battle_pack = capabilities::BattlePackIdentity();
    const TypeRef context = TypeRef::Named(
        capabilities::BattleContextSchemaIdentity());
    const TypeRef request = TypeRef::Named(
        capabilities::BattleCaptureContextRequestSchemaIdentity());
    return {
        .canonical_id = "soa.battle.context.capture_at_turn_inputs",
        .revision = 1,
        .source_name = "BattleContextModule",
        .await = {
            .alternatives = {{
                .capability_pack = battle_pack,
                .canonical_id = "soa.battle.point.TurnInputs",
                .kind = SemanticPointKind::ProgramCounter,
                .physical_pc = TurnInputsPc,
            }},
            .current_point = CurrentPointPolicy::FutureOnly,
            .movie_policy = SemanticMoviePolicy::FailIfEnded,
            .continue_until_action = CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil),
            .receipt_type = CanonicalActionOutputType(
                CanonicalAction::ExecutionContinueUntil),
        },
        .observations = {{
            .canonical_id = "battle_context",
            .result_type = context,
            .permitted_modes = {
                ObservationAcquisitionMode::PausedAtPoint},
            .paused_action =
                capabilities::BattleCaptureContextActionIdentity(),
            .paused_action_pack = battle_pack,
            .paused_request_type = request,
            .paused_request_fields = {
                {
                    .field_name = "workset_epoch",
                    .field_type = TypeRef::Builtin(BuiltinType::U64),
                    .source = ObservationDefinition::
                        RequestValueSource::StopReceiptField,
                    .source_field = "workset_epoch",
                },
                {
                    .field_name = "expected_pc",
                    .field_type = TypeRef::Builtin(BuiltinType::U32),
                    .source = ObservationDefinition::
                        RequestValueSource::StopReceiptField,
                    .source_field = "pc",
                },
            },
            .coherent_query = true,
        }},
        .ordered_uses = {{
            .canonical_id = "capture",
            .definition_id = "battle_context",
            .mode = ObservationAcquisitionMode::PausedAtPoint,
            .requirement = ObservationRequirement::Required,
        }},
        .output_type = context,
    };
}

void AddCanonicalActionImports(
    program::composition::detail::ModuleFragmentBuilder& builder,
    CanonicalAction action)
{
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const SchemaIdentity& schema :
         CanonicalActionTypeSchemaClosure(action))
    {
        builder.AddTypeImport(schema);
    }
}

std::optional<ProgramValueId> AddPausedPcCheck(
    program::composition::detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint32_t expected_pc,
    std::string selector)
{
    const auto expected = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        TypeRef::Builtin(BuiltinType::U64),
        {},
        {},
        selector + "/expected-pc",
        LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::U64),
            .payload = static_cast<std::uint64_t>(expected_pc),
        });
    const auto request = expected
        ? builder.AddInstruction(
              function,
              block,
              InstructionOpcode::RecordConstruct,
              CanonicalActionInputType(
                  CanonicalAction::ExecutionRequirePausedPc),
              std::array{*expected},
              {},
              selector + "/request")
        : std::nullopt;
    return request
        ? builder.AddInstruction(
              function,
              block,
              InstructionOpcode::AwaitAction,
              CanonicalActionOutputType(
                  CanonicalAction::ExecutionRequirePausedPc),
              std::array{*request},
              {
                  .kind = InstructionTargetKind::Action,
                  .dependency = CanonicalActionIdentity(
                      CanonicalAction::ExecutionRequirePausedPc),
              },
              selector + "/require")
        : std::nullopt;
}

ProgramModule ConstructBattleContextModuleV1()
{
    ProgramModule module;
    module.identity = {
        .canonical_id = std::string(ModuleCanonicalId),
        .revision = 1,
    };

    const CompositionResult observation =
        LowerSemanticObservation(
            BattleContextObservation(),
            module);
    if (!observation || !observation.function)
    {
        throw std::logic_error(
            observation.diagnostics.empty()
                ? "Battle Context observation lowering failed"
                : observation.diagnostics.front().message);
    }

    program::composition::detail::ModuleFragmentBuilder builder(
        module,
        "BattleContextModule",
        "battle-context/capture/v1");
    AddCanonicalActionImports(
        builder,
        CanonicalAction::ExecutionRequirePausedPc);
    builder.AddTypeImport(
        capabilities::BattleContextSchemaIdentity());
    builder.AddLocalType({
        .identity = BattleContextCaptureResultSchemaIdentityV1(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"context", TypeRef::Named(
                 capabilities::BattleContextSchemaIdentity())},
            {"entry_pc", TypeRef::Builtin(BuiltinType::U32)},
            {"entry_vi_count", TypeRef::Builtin(BuiltinType::U64)},
            {"entry_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"capture_pc", TypeRef::Builtin(BuiltinType::U32)},
            {"capture_vi_count", TypeRef::Builtin(BuiltinType::U64)},
            {"capture_epoch", TypeRef::Builtin(BuiltinType::U64)},
        },
    });

    const ValueDefinition input = builder.NewArgument(
        TypeRef::Builtin(BuiltinType::Unit));
    auto& function = builder.AddFunction(
        std::string(Entrypoint),
        std::array{input},
        CaptureResultType(),
        TypeRef::Builtin(BuiltinType::Bool),
        true);
    auto& block = builder.AddBlock(function);
    const auto entry = AddPausedPcCheck(
        builder,
        function,
        block,
        BeforeRandSeedSetPc,
        "entry");
    const auto context = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::CallLocal,
        TypeRef::Named(
            capabilities::BattleContextSchemaIdentity()),
        {},
        {
            .kind = InstructionTargetKind::LocalFunction,
            .local_function = *observation.function,
        },
        "capture/context");
    const auto capture = AddPausedPcCheck(
        builder,
        function,
        block,
        TurnInputsPc,
        "capture");
    if (!entry || !context || !capture)
        throw std::logic_error("Battle Context entrypoint lowering failed");

    const auto project = [&](ProgramValueId receipt,
                             TypeRef type,
                             std::string field) {
        return builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordProject,
            type,
            std::array{receipt},
            {},
            std::move(field));
    };
    const auto entry_pc = project(
        *entry,
        TypeRef::Builtin(BuiltinType::U32),
        "pc");
    const auto entry_vi = project(
        *entry,
        TypeRef::Builtin(BuiltinType::U64),
        "vi_count");
    const auto entry_epoch = project(
        *entry,
        TypeRef::Builtin(BuiltinType::U64),
        "workset_epoch");
    const auto capture_pc = project(
        *capture,
        TypeRef::Builtin(BuiltinType::U32),
        "pc");
    const auto capture_vi = project(
        *capture,
        TypeRef::Builtin(BuiltinType::U64),
        "vi_count");
    const auto capture_epoch = project(
        *capture,
        TypeRef::Builtin(BuiltinType::U64),
        "workset_epoch");
    if (!entry_pc || !entry_vi || !entry_epoch ||
        !capture_pc || !capture_vi || !capture_epoch)
    {
        throw std::logic_error(
            "Battle Context provenance projection failed");
    }
    const std::array result_fields{
        *context,
        *entry_pc,
        *entry_vi,
        *entry_epoch,
        *capture_pc,
        *capture_vi,
        *capture_epoch,
    };
    const auto result = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordConstruct,
        CaptureResultType(),
        result_fields,
        {},
        "result");
    const auto succeeded = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        TypeRef::Builtin(BuiltinType::Bool),
        {},
        {},
        "domain/succeeded",
        LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::Bool),
            .payload = true,
        });
    if (!result || !succeeded)
        throw std::logic_error("Battle Context result lowering failed");
    builder.SetTerminator(
        function,
        block,
        {
            .kind = TerminatorKind::Return,
            .return_value = *result,
            .domain_outcome = *succeeded,
        },
        "return");

    module.accepted_policies = {
        .state_policies = {
            InvocationStatePolicy::RestoreBaseline},
        .execution_intents = {ExecutionIntent::Live},
    };
    module.budgets = {
        .maximum_instructions = 128,
        .maximum_calls = 8,
        .maximum_call_depth = 4,
        .maximum_action_requests = 12,
        .maximum_emissions = 1,
        .maximum_artifacts = 1,
        .maximum_values = 512,
        .maximum_value_bytes = 1024u * 1024u,
        .maximum_trace_events = 256,
    };
    module.entrypoints = {{
        .name = std::string(Entrypoint),
        .function = function.id,
        .input_type = TypeRef::Builtin(BuiltinType::Unit),
        .output_type = CaptureResultType(),
        .domain_outcome_type =
            TypeRef::Builtin(BuiltinType::Bool),
        .required_capability_packs =
            module.required_capability_packs,
        .accepted_policies = module.accepted_policies,
    }};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

std::optional<ProgramDependencyLock> VerifyModule(
    const ProgramModule& module,
    std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const RegistryResult source =
        capabilities::RegisterSourceCapabilityPacks(
            schemas,
            actions,
            packs);
    if (!source.success)
    {
        SetDiagnostic(diagnostic, source.error.message);
        return std::nullopt;
    }
    const ModuleStoreResult stored =
        modules.RegisterCompiled(module);
    if (!stored.success)
    {
        SetDiagnostic(diagnostic, stored.error.message);
        return std::nullopt;
    }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const ProgramVerificationResult verified = verifier.Verify(
        stored.module->identity,
        capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        SetDiagnostic(
            diagnostic,
            verified.diagnostics.empty()
                ? "Battle Context module verification failed"
                : verified.diagnostics.front().message);
        return std::nullopt;
    }
    return verified.verified->dependency_lock;
}

ProgramValueGraph UnitInput()
{
    ProgramValue value{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::Unit),
        UnitValue{}};
    return {value.id, {std::move(value)}};
}

const ProgramValue* FindValue(
    const ProgramValueGraph& graph,
    ProgramValueId id)
{
    const auto found = std::ranges::find(
        graph.values,
        id,
        &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

bool DecodeCaptureResult(
    const ProgramValueGraph& graph,
    BattleContextCaptureResultV1& output,
    std::string* diagnostic)
{
    const ProgramValue* root = FindValue(graph, graph.root);
    const auto* fields = root && root->type == CaptureResultType()
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    if (!fields || fields->fields.size() != 7)
    {
        SetDiagnostic(diagnostic, "Battle Context result record is malformed");
        return false;
    }
    ProgramValueGraph context_graph = graph;
    context_graph.root = fields->fields[0];
    BattleContextCaptureResultV1 decoded;
    if (!capabilities::DecodeBattleContextValue(
            context_graph,
            decoded.context))
    {
        SetDiagnostic(diagnostic, "Battle Context value is malformed");
        return false;
    }
    const auto scalar = [&](std::size_t index, auto& value) {
        const ProgramValue* field = FindValue(
            graph,
            fields->fields[index]);
        using Value = std::remove_reference_t<decltype(value)>;
        const auto* payload = field
            ? std::get_if<Value>(&field->payload)
            : nullptr;
        if (!payload) return false;
        value = *payload;
        return true;
    };
    std::uint64_t entry_epoch = 0;
    std::uint64_t capture_epoch = 0;
    if (!scalar(1, decoded.entry_pc) ||
        !scalar(2, decoded.entry_vi_count) ||
        !scalar(3, entry_epoch) ||
        !scalar(4, decoded.capture_pc) ||
        !scalar(5, decoded.capture_vi_count) ||
        !scalar(6, capture_epoch))
    {
        SetDiagnostic(diagnostic, "Battle Context provenance is malformed");
        return false;
    }
    decoded.entry_epoch = WorksetEpoch(entry_epoch);
    decoded.capture_epoch = WorksetEpoch(capture_epoch);
    output = std::move(decoded);
    return true;
}

class BattleContextFullPhaseDefinition final
    : public IBattleContextFullPhaseDefinitionV1
{
public:
    BattleContextFullPhaseDefinition()
    {
        std::string diagnostic;
        const ProgramModule module =
            ConstructBattleContextModuleV1();
        const auto dependencies = VerifyModule(
            module,
            &diagnostic);
        const EncodeResult encoded =
            EncodeProgramModuleV1(module);
        if (!dependencies || !encoded)
        {
            throw std::logic_error(
                "Battle Context Full Phase definition is invalid: " +
                (diagnostic.empty()
                    ? encoded.status.message
                    : diagnostic));
        }
        dependencies_ = *dependencies;
        runtime_profile_ = InvocationRuntimeProfile(*dependencies);
        module_identity_ = module.identity;
        module_envelope_ = {
            .identity = {
                .canonical_id = module.identity.canonical_id,
                .revision = module.identity.revision,
                .canonical_hash = module.identity.module_hash.ToHex(),
            },
            .format_version = kProgramCodecVersionV1,
            .development_only = false,
            .payload = encoded.bytes,
        };
        const ContentHash256 dependency_hash =
            ComputeProgramDependencyLockHashV1(*dependencies);
        const InvocationExecutionPolicy execution{
            .intent = ExecutionIntent::Live,
            .allow_input = false,
            .allow_capture = false,
            .record_trace = false,
        };
        const ProgramInvocation sample = Resolve(
            ProgramExecutionId(1),
            AttemptId(1),
            execution,
            module.budgets);
        constexpr std::string_view movie_policy =
            "soa.battle.context/no-movie/v1";
        constexpr std::string_view service_policy =
            "soa.battle.context/exact-pc-turn-inputs/v1";
        runtime_ = {
            .module = module_envelope_.identity,
            .entrypoint = std::string(Entrypoint),
            .dependency_lock_sha256 = dependency_hash.ToHex(),
            .verified_dependency_sha256 =
                ComputeProgramInvocationCompatibilityHashV1(sample),
            .runtime_profile_sha256 =
                RuntimeProfileHash(runtime_profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = execution,
            .limits = module.budgets,
            .baseline_lineage = std::string(BaselineLineage),
            .movie_policy_sha256 = hash::sha256(
                movie_policy.data(), movie_policy.size()),
            .service_policy_sha256 = hash::sha256(
                service_policy.data(), service_policy.size()),
        };
        std::string canonical =
            std::string(FullPhaseCanonicalId) + "\0" +
            runtime_.module.canonical_hash + "\0" +
            runtime_.verified_dependency_sha256 + "\0" +
            runtime_.service_policy_sha256;
        identity_ = {
            .program_kind = static_cast<std::int32_t>(
                savor::PK_BattleContext),
            .program_version = ProgramVersion,
            .canonical_id = std::string(FullPhaseCanonicalId),
            .contract_revision = 1,
            .canonical_sha256 = hash::sha256(
                canonical.data(), canonical.size()),
        };
    }

    const fullphase::FullPhaseProgramIdentity& identity()
        const noexcept override { return identity_; }
    const fullphase::FullPhaseRuntimeContract& runtime_contract()
        const noexcept override { return runtime_; }
    const EncodedModuleEnvelope& module_envelope()
        const noexcept override { return module_envelope_; }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic) const override
    {
        if (!DecodeBattleContextExecutionInputV1(
                input_payload,
                diagnostic) || !execution_id || !attempt_id)
        {
            if (diagnostic && diagnostic->empty())
                *diagnostic = "Battle Context execution identity is invalid";
            return std::nullopt;
        }
        return Resolve(
            execution_id,
            attempt_id,
            runtime_.execution,
            runtime_.limits);
    }

    bool DecodeProgramResult(
        std::span<const Byte> encoded_result,
        BattleContextCaptureResultV1& result,
        std::string* diagnostic) const override
    {
        const DecodeResult<ProgramResult> decoded =
            DecodeProgramResultV1(encoded_result);
        if (!decoded || !decoded.value)
        {
            SetDiagnostic(
                diagnostic,
                decoded.status.message.empty()
                    ? "Battle Context ProgramResult did not complete"
                    : decoded.status.message);
            return false;
        }
        const ProgramResult& value = *decoded.value;
        if (value.module != module_identity_ ||
            value.entrypoint != Entrypoint ||
            value.resolved_dependencies != dependencies_)
        {
            SetDiagnostic(
                diagnostic,
                "Battle Context ProgramResult does not match the admitted definition");
            return false;
        }
        if (value.infrastructure !=
                ProgramInfrastructureStatus::Completed ||
            value.cleanup != ProgramCleanupStatus::Clean ||
            value.session_disposition != SessionDisposition::Clean ||
            !value.output || !value.domain_outcome)
        {
            SetDiagnostic(
                diagnostic,
                "Battle Context ProgramResult did not complete cleanly");
            return false;
        }
        const ProgramValue* domain = FindValue(
            *value.domain_outcome,
            value.domain_outcome->root);
        const auto* succeeded = domain
            ? std::get_if<bool>(&domain->payload)
            : nullptr;
        if (!domain ||
            domain->type != TypeRef::Builtin(BuiltinType::Bool) ||
            !succeeded || !*succeeded)
        {
            SetDiagnostic(
                diagnostic,
                "Battle Context ProgramResult domain outcome is not true");
            return false;
        }
        return DecodeCaptureResult(
            *value.output,
            result,
            diagnostic);
    }

private:
    ProgramInvocation Resolve(
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        const InvocationExecutionPolicy& execution,
        const ProgramBudgets& limits) const
    {
        return {
            .invocation_id = execution_id,
            .attempt_id = attempt_id,
            .module = module_identity_,
            .entrypoint = std::string(Entrypoint),
            .dependencies = dependencies_,
            .runtime_profile = runtime_profile_,
            .state = {
                .policy = InvocationStatePolicy::RestoreBaseline,
                .session_lineage = std::string(BaselineLineage),
            },
            .execution = execution,
            .input = UnitInput(),
            .limits = limits,
            .provenance = {
                .requesting_component = "SavorDb.battle.context",
                .attributes = {{
                    "contract",
                    "soa.battle.context/capture@1",
                }},
            },
        };
    }

    fullphase::FullPhaseProgramIdentity identity_;
    fullphase::FullPhaseRuntimeContract runtime_;
    EncodedModuleEnvelope module_envelope_;
    ModuleIdentity module_identity_;
    ProgramDependencyLock dependencies_;
    RuntimeProfile runtime_profile_;
};

} // namespace

program::SchemaIdentity BattleContextCaptureResultSchemaIdentityV1()
{
    const TypeRef context = TypeRef::Named(
        capabilities::BattleContextSchemaIdentity());
    return program::composition::ExactSchema(
        "soa.battle.context.CaptureResult",
        1,
        "record BattleContextCaptureResult/1(context:" +
            TypeContract(context) +
            ",entry_pc:u32,entry_vi_count:u64,entry_epoch:u64,capture_pc:u32,capture_vi_count:u64,capture_epoch:u64)");
}

std::vector<std::uint8_t> EncodeBattleContextExecutionInputV1()
{
    return {1};
}

bool DecodeBattleContextExecutionInputV1(
    std::span<const std::uint8_t> input,
    std::string* diagnostic)
{
    if (input.size() != 1 || input.front() != 1)
    {
        SetDiagnostic(
            diagnostic,
            "Battle Context execution input must be wire version 1");
        return false;
    }
    SetDiagnostic(diagnostic, {});
    return true;
}

std::shared_ptr<const IBattleContextFullPhaseDefinitionV1>
BattleContextFullPhaseDefinitionV1()
{
    static const auto definition =
        std::make_shared<const BattleContextFullPhaseDefinition>();
    return definition;
}

} // namespace savor::runtime::battlecontext
