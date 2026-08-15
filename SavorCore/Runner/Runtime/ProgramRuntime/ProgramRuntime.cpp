#include "ProgramRuntime.h"
#include "../Worksets/WorksetTypes.h"

#include "Capabilities/SourceCapabilityPacks.h"
#include "Capabilities/SourceReducers.h"
#include "Codec/ProgramCodecV1.h"
#include "Model/ProgramValueArena.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace savor::runtime::program {
namespace {

constexpr ProgramScopeId kInvocationScope{
    std::numeric_limits<std::uint64_t>::max()};

[[nodiscard]] std::optional<ModuleIdentity> ModuleIdentityFromEnvelope(
    const ProgramModuleIdentity& identity)
{
    const std::optional<ContentHash256> hash =
        ContentHash256::FromHex(identity.canonical_hash);
    if (identity.canonical_id.empty() || identity.revision == 0 ||
        !hash)
    {
        return std::nullopt;
    }
    return ModuleIdentity{
        identity.canonical_id,
        identity.revision,
        *hash};
}

[[nodiscard]] ProgramModuleIdentity ModuleIdentityToEnvelope(
    const ModuleIdentity& identity)
{
    return {
        identity.canonical_id,
        identity.revision,
        identity.module_hash.ToHex()};
}

[[nodiscard]] ProgramValueGraph UnitGraph()
{
    return {
        ProgramValueId(1),
        {ProgramValue{
            ProgramValueId(1),
            TypeRef::Builtin(BuiltinType::Unit),
            UnitValue{}}}};
}

[[nodiscard]] const ProgramValue* RootValue(
    const ProgramValueGraph& graph) noexcept
{
    const auto found = std::ranges::find(
        graph.values,
        graph.root,
        &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramEntrypoint* FindEntrypoint(
    const ProgramModule& module,
    std::string_view name) noexcept
{
    const auto found = std::ranges::find(
        module.entrypoints,
        name,
        &ProgramEntrypoint::name);
    return found == module.entrypoints.end() ? nullptr : &*found;
}

[[nodiscard]] bool BudgetsWithin(
    const ProgramBudgets& requested,
    const ProgramBudgets& maximum) noexcept
{
    const auto within = [](std::uint64_t value, std::uint64_t limit) {
        return value != 0 && limit != 0 && value <= limit;
    };
    return within(
               requested.maximum_instructions,
               maximum.maximum_instructions) &&
        within(requested.maximum_calls, maximum.maximum_calls) &&
        within(
               requested.maximum_call_depth,
               maximum.maximum_call_depth) &&
        within(
               requested.maximum_action_requests,
               maximum.maximum_action_requests) &&
        requested.maximum_emissions <= maximum.maximum_emissions &&
        requested.maximum_artifacts <= maximum.maximum_artifacts &&
        within(requested.maximum_values, maximum.maximum_values) &&
        within(
               requested.maximum_value_bytes,
               maximum.maximum_value_bytes) &&
        within(
               requested.maximum_trace_events,
               maximum.maximum_trace_events);
}

[[nodiscard]] bool InvocationPolicyAccepted(
    const ProgramInvocation& invocation,
    const ProgramEntrypoint& entrypoint) noexcept
{
    const ProgramPolicySet& accepted = entrypoint.accepted_policies;
    if (!std::ranges::contains(
            accepted.state_policies,
            invocation.state.policy) ||
        !std::ranges::contains(
            accepted.execution_intents,
            invocation.execution.intent))
    {
        return false;
    }
    if ((invocation.execution.allow_movie_playback &&
         !accepted.permits_movie_playback) ||
        (invocation.execution.allow_movie_recording &&
         !accepted.permits_movie_recording) ||
        (invocation.execution.allow_capture &&
         !accepted.permits_capture) ||
        (invocation.execution.intent == ExecutionIntent::Replay &&
         !accepted.permits_replay))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool InvocationStateShapeAccepted(
    const InvocationStateRequest& state) noexcept
{
    return state.policy == InvocationStatePolicy::RestoreBaseline ||
        state.policy == InvocationStatePolicy::EstablishBaseline;
}

[[nodiscard]] bool RuntimeProfileConfigurationAccepted(
    const ProgramRuntimeConfig& config) noexcept
{
    const RuntimeProfile& profile = config.runtime_profile;
    return !config.compatibility.game_id.empty() &&
        !config.compatibility.executable_identity.empty() &&
        !config.compatibility.address_map_revision.empty() &&
        !profile.profile_id.empty() &&
        profile.game_id == config.compatibility.game_id &&
        !profile.disc_identity.empty() &&
        profile.executable_identity ==
            config.compatibility.executable_identity &&
        !profile.backend.empty() &&
        profile.capability_packs.empty() &&
        config.bounded_host_operation_timeout >
            std::chrono::milliseconds::zero();
}

[[nodiscard]] bool RuntimeProfileAccepted(
    const RuntimeProfile& profile,
    const ProgramRuntimeConfig& config,
    const ProgramDependencyLock& dependencies) noexcept
{
    const RuntimeProfile& expected = config.runtime_profile;
    return profile.profile_id == expected.profile_id &&
        profile.game_id == expected.game_id &&
        profile.disc_identity == expected.disc_identity &&
        profile.executable_identity ==
            expected.executable_identity &&
        profile.backend == expected.backend &&
        profile.capability_packs ==
            dependencies.capability_packs;
}

[[nodiscard]] ActionEffectMask AllowedEffects(
    const InvocationExecutionPolicy& policy) noexcept
{
    ActionEffectMask allowed = ~ActionEffectMask{0};
    if (!policy.allow_input)
        allowed &= ~EffectMask(ActionEffect::PublishInput);
    if (!policy.allow_movie_playback)
        allowed &= ~EffectMask(ActionEffect::MoviePlayback);
    if (!policy.allow_movie_recording)
        allowed &= ~EffectMask(ActionEffect::MovieRecording);
    if (!policy.allow_capture)
        allowed &= ~EffectMask(ActionEffect::Capture);
    return allowed;
}

[[nodiscard]] InvocationTerminalStatus MapTerminal(
    ProgramInfrastructureStatus status) noexcept
{
    switch (status)
    {
    case ProgramInfrastructureStatus::Completed:
        return InvocationTerminalStatus::Completed;
    case ProgramInfrastructureStatus::Cancelled:
        return InvocationTerminalStatus::Cancelled;
    case ProgramInfrastructureStatus::TimedOut:
    case ProgramInfrastructureStatus::BudgetExhausted:
        return InvocationTerminalStatus::TimedOut;
    case ProgramInfrastructureStatus::BackendFailed:
        return InvocationTerminalStatus::InfrastructureFailure;
    case ProgramInfrastructureStatus::Rejected:
    case ProgramInfrastructureStatus::ContractFailed:
        return InvocationTerminalStatus::Failed;
    }
    return InvocationTerminalStatus::Failed;
}

[[nodiscard]] CleanupStatus MapCleanup(
    ProgramCleanupStatus status) noexcept
{
    switch (status)
    {
    case ProgramCleanupStatus::Clean:
        return CleanupStatus::Clean;
    case ProgramCleanupStatus::CleanWithDiagnostics:
        return CleanupStatus::CleanWithDiagnostics;
    case ProgramCleanupStatus::Tainted:
        return CleanupStatus::Failed;
    }
    return CleanupStatus::Failed;
}

[[nodiscard]] WorkerRejectionCode MapDiagnostic(
    ProgramInfrastructureStatus status) noexcept
{
    switch (status)
    {
    case ProgramInfrastructureStatus::Completed:
        return WorkerRejectionCode::None;
    case ProgramInfrastructureStatus::Cancelled:
    case ProgramInfrastructureStatus::TimedOut:
    case ProgramInfrastructureStatus::BudgetExhausted:
        return WorkerRejectionCode::InvalidState;
    case ProgramInfrastructureStatus::Rejected:
    case ProgramInfrastructureStatus::ContractFailed:
        return WorkerRejectionCode::InvalidArgument;
    case ProgramInfrastructureStatus::BackendFailed:
        return WorkerRejectionCode::BackendFailure;
    }
    return WorkerRejectionCode::InternalFailure;
}

} // namespace

std::string ComputeProgramInvocationCompatibilityHashV1(
    const ProgramInvocation& source)
{
    ProgramInvocation invocation = source;
    invocation.invocation_id = {};
    invocation.attempt_id = {};
    invocation.state.expected_session = {};
    invocation.state.expected_epoch = {};
    invocation.state.session_lineage.clear();
    // Use a valid, type-neutral graph as the canonical placeholder. An empty
    // graph is not encodable and per-item input must not affect this key.
    invocation.input = UnitGraph();
    invocation.limits = {};
    invocation.provenance = {};
    const EncodeResult encoded = EncodeProgramInvocationV1(invocation);
    if (!encoded)
        return {};
    return hash::sha256(encoded.bytes.data(), encoded.bytes.size());
}

struct ProgramRuntime::Impl
{
    enum class ActiveStage : std::uint8_t
    {
        PreparingState,
        Executing,
        ExecutionFinished,
    };

    struct ActiveInvocation
    {
        ProgramInvocation invocation;
        WorksetEpoch workset_epoch;
        std::shared_ptr<const VerifiedProgramModule> verified;
        CancellationToken cancellation;
        std::shared_ptr<IProgramRuntimeEventSink> events;
        ActiveStage stage = ActiveStage::PreparingState;
        ProgramActionRequestId state_request;
        std::unique_ptr<ProgramExecutor> executor;
        CancellationReason pending_cancellation =
            CancellationReason::None;
        ProgramCleanupStatus preparation_cleanup =
            ProgramCleanupStatus::Clean;
        SessionDisposition preparation_disposition =
            SessionDisposition::Clean;
        std::vector<CleanupReceipt>
            preparation_cleanup_receipts;
        std::optional<ProgramExecutionFinished> finished;
    };

    struct PreparedTemplate
    {
        ProgramInvocation invocation;
        std::shared_ptr<const VerifiedProgramModule> verified;
        std::string compatibility_sha256;
    };

    explicit Impl(ProgramRuntimeConfig config)
        : config(std::move(config)),
          actions(&types),
          packs(&types, &actions),
          verifier(definitions, types, actions, packs)
    {
        if (!RuntimeProfileConfigurationAccepted(this->config))
        {
            initialization_diagnostic =
                "ProgramRuntime runtime-profile constraints are invalid";
            return;
        }
        const RegistryResult registered =
            capabilities::RegisterSourceCapabilityPacks(
                types,
                actions,
                packs);
        initialized = registered.success;
        if (!registered.success)
        {
            initialization_diagnostic =
                registered.error.message.empty()
                ? "Source capability packs could not be registered"
                : registered.error.message;
        }
    }

    [[nodiscard]] ProgramActionRequestId NextActionId()
    {
        if (next_action_id == 0 ||
            next_action_id ==
                std::numeric_limits<std::uint64_t>::max())
        {
            return {};
        }
        return ProgramActionRequestId(next_action_id++);
    }

    void FinishExecution(ProgramResult result)
    {
        if (!active ||
            active->stage == ActiveStage::ExecutionFinished)
        {
            return;
        }
        active->stage = ActiveStage::ExecutionFinished;
        if (active->preparation_cleanup > result.cleanup)
            result.cleanup = active->preparation_cleanup;
        if (active->preparation_disposition >
            result.session_disposition)
        {
            result.session_disposition =
                active->preparation_disposition;
        }
        for (const CleanupReceipt& receipt :
             active->preparation_cleanup_receipts)
        {
            if (receipt.status > result.cleanup)
                result.cleanup = receipt.status;
            if (receipt.status ==
                ProgramCleanupStatus::Tainted)
            {
                result.session_disposition =
                    SessionDisposition::Tainted;
            }
        }
        result.cleanup_receipts.insert(
            result.cleanup_receipts.begin(),
            std::make_move_iterator(
                active->preparation_cleanup_receipts.begin()),
            std::make_move_iterator(
                active->preparation_cleanup_receipts.end()));
        const EncodeResult encoded = EncodeProgramResultV1(result);

        ProgramExecutionFinished finished;
        finished.invocation_id = result.invocation_id;
        finished.attempt_id = result.attempt_id;
        finished.status = MapTerminal(result.infrastructure);
        finished.cleanup = MapCleanup(result.cleanup);
        finished.session_disposition = result.session_disposition;
        finished.workset_epoch = active->workset_epoch;
        if (encoded)
            finished.output_payload = encoded.bytes;
        else
        {
            finished.status =
                InvocationTerminalStatus::InfrastructureFailure;
            finished.cleanup = CleanupStatus::Failed;
            finished.session_disposition = SessionDisposition::Tainted;
            finished.error = {
                WorkerRejectionCode::InternalFailure,
                encoded.status.message.empty()
                    ? "ProgramResult canonical encoding failed"
                    : encoded.status.message};
        }
        if (!finished.error && result.infrastructure !=
                ProgramInfrastructureStatus::Completed)
        {
            finished.error = {
                MapDiagnostic(result.infrastructure),
                result.diagnostics.empty()
                    ? "Canonical program invocation did not complete"
                    : result.diagnostics.back().message};
        }
        active->finished = std::move(finished);
    }

    ProgramRuntimeConfig config;
    TypeSchemaRegistry types;
    ActionRegistry actions;
    CapabilityPackRegistry packs;
    ProgramDefinitionStore definitions;
    ProgramVerifier verifier;
    std::shared_ptr<IProgramActionRequestSink> action_sink;
    std::optional<ActiveInvocation> active;
    std::map<std::uint64_t, PreparedTemplate> prepared_templates;
    std::uint64_t next_action_id = 1;
    std::uint64_t next_template_id = 1;
    bool initialized = false;
    bool shutdown = false;
    std::string initialization_diagnostic;
};

ProgramRuntime::ProgramRuntime(ProgramRuntimeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

ProgramRuntime::~ProgramRuntime() = default;

bool ProgramRuntime::initialized() const noexcept
{
    return impl_->initialized;
}

const std::string& ProgramRuntime::initialization_diagnostic()
    const noexcept
{
    return impl_->initialization_diagnostic;
}

ProgramRuntimeSubmission ProgramRuntime::AdmitModuleClosure(
    ModuleClosureAdmissionRequest request,
    ModuleClosureAdmissionReceipt& receipt)
{
    receipt = {};
    if (impl_->shutdown || !impl_->initialized)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::ProgramRuntimeUnavailable,
            impl_->initialization_diagnostic.empty()
                ? "Canonical ProgramRuntime is unavailable"
                : impl_->initialization_diagnostic);
    }
    if (request.root.canonical_id.empty() ||
        request.root.revision == 0 ||
        request.expected_dependency_lock_sha256.size() != 64 ||
        request.modules.empty() || request.modules.size() > 64)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Workset module closure admission request is incomplete");
    }

    ProgramDefinitionStore candidate = impl_->definitions;
    std::map<std::pair<std::string, std::uint32_t>,
             ProgramModuleIdentity> supplied;
    for (const EncodedModuleEnvelope& envelope : request.modules)
    {
        if (envelope.format_version != kProgramCodecVersionV1 ||
            envelope.payload.empty())
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "Workset module closure contains an invalid envelope");
        }
        const auto expected =
            ModuleIdentityFromEnvelope(envelope.identity);
        if (!expected)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "Workset module closure contains an invalid identity");
        }
        const auto key = std::pair{
            envelope.identity.canonical_id,
            envelope.identity.revision};
        if (!supplied.emplace(key, envelope.identity).second)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "Workset module closure contains duplicate module identities");
        }
        const ModuleStoreResult stored = candidate.RegisterEncoded(
            envelope.payload,
            expected);
        if (!stored.success)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                stored.error.message.empty()
                    ? "Workset module closure could not be decoded"
                    : stored.error.message);
        }
    }

    const auto root = ModuleIdentityFromEnvelope(request.root);
    RegistryError closure_error;
    const auto closure = root
        ? candidate.ResolveClosure(*root, &closure_error)
        : std::nullopt;
    if (!closure || closure->dependency_order.size() != supplied.size())
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            closure_error.message.empty()
                ? "Workset module package is not an exact closed dependency set"
                : closure_error.message);
    }
    for (const auto& module : closure->dependency_order)
    {
        const auto found = supplied.find({
            module->identity.canonical_id,
            module->identity.revision});
        if (found == supplied.end() ||
            found->second.canonical_hash !=
                module->identity.module_hash.ToHex())
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "Workset module package dependency closure is not exact");
        }
    }

    ProgramVerifier verifier(
        candidate,
        impl_->types,
        impl_->actions,
        impl_->packs);
    std::string root_dependency_hash;
    for (const auto& module : closure->dependency_order)
    {
        ProgramVerificationResult verified = verifier.Verify(
            module->identity,
            impl_->config.compatibility);
        if (!verified.success)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                verified.diagnostics.empty()
                    ? "Workset module verification failed"
                    : verified.diagnostics.front().message);
        }
        const ContentHash256 dependency_hash =
            ComputeProgramDependencyLockHashV1(
                verified.verified->dependency_lock);
        if (dependency_hash.empty())
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "Workset module dependency-lock hash could not be computed");
        }
        if (module->identity == *root)
            root_dependency_hash = dependency_hash.ToHex();
    }
    if (root_dependency_hash !=
        request.expected_dependency_lock_sha256)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Workset root dependency-lock hash does not match its package");
    }

    impl_->definitions = std::move(candidate);
    receipt = {
        .root = std::move(request.root),
        .dependency_lock_sha256 = std::move(root_dependency_hash),
        .admitted_module_count = request.modules.size(),
    };
    return ProgramRuntimeSubmission::Accepted();
}

ProgramRuntimeSubmission ProgramRuntime::StartInvocation(
    InternalProgramInvocationStartRequest request,
    CancellationToken cancellation,
    std::shared_ptr<IProgramRuntimeEventSink> events)
{
    if (impl_->shutdown || !impl_->initialized)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::ProgramRuntimeUnavailable,
            "Canonical ProgramRuntime is unavailable");
    }
    if (impl_->active)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvocationAlreadyActive,
            "Canonical ProgramRuntime already owns an invocation");
    }
    if (!events || !impl_->action_sink)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::ProgramRuntimeUnavailable,
            "Canonical ProgramRuntime has no actor action sink");
    }
    if (request.state_already_prepared &&
        request.prepared_baseline_sha256.size() != 64)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Prepared invocation requires a canonical baseline hash");
    }

    DecodeResult<ProgramInvocation> decoded =
        DecodeProgramInvocationV1(request.invocation.input_payload);
    if (!decoded)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            decoded.status.message.empty()
                ? "SPRI invocation payload is invalid"
                : decoded.status.message);
    }
    ProgramInvocation invocation = std::move(*decoded.value);
    const auto outer_module =
        ModuleIdentityFromEnvelope(request.invocation.module);
    if (!outer_module ||
        invocation.invocation_id !=
            request.invocation.invocation_id ||
        invocation.attempt_id != request.invocation.attempt_id ||
        invocation.module != *outer_module ||
        invocation.entrypoint != request.invocation.entrypoint ||
        invocation.state.expected_epoch !=
            request.invocation.expected_workset_epoch ||
        cancellation.invocation_id() != invocation.invocation_id)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "SPRI invocation does not match its worker envelope");
    }

    ProgramVerificationResult verified = impl_->verifier.Verify(
        invocation.module,
        impl_->config.compatibility);
    if (!verified.success)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            verified.diagnostics.empty()
                ? "Program invocation module verification failed"
                : verified.diagnostics.front().message);
    }

    const ProgramEntrypoint* entrypoint =
        FindEntrypoint(
            *verified.verified->module,
            invocation.entrypoint);
    const ProgramValue* input = RootValue(invocation.input);
    const ProgramBudgets* maximum_budgets = entrypoint &&
            entrypoint->narrowed_budgets
        ? &*entrypoint->narrowed_budgets
        : &verified.verified->module->budgets;
    if (!entrypoint || !input ||
        input->type != entrypoint->input_type ||
        invocation.dependencies !=
            verified.verified->dependency_lock ||
        !RuntimeProfileAccepted(
            invocation.runtime_profile,
            impl_->config,
            verified.verified->dependency_lock) ||
        !BudgetsWithin(invocation.limits, *maximum_budgets) ||
        !InvocationPolicyAccepted(invocation, *entrypoint) ||
        !invocation.state.expected_session ||
        invocation.state.session_lineage.empty() ||
        !InvocationStateShapeAccepted(invocation.state))
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "SPRI invocation violates its verified entrypoint, "
            "compatibility, dependency, state, or budget contract");
    }
    const ProgramValueArenaStatus input_status =
        ValidateProgramValueGraph(
            invocation.input,
            entrypoint->input_type,
            verified.verified->type_closure,
            {
                invocation.limits.maximum_values,
                invocation.limits.maximum_value_bytes,
            },
            invocation.state.expected_epoch);
    if (!input_status)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            input_status.message.empty()
                ? "SPRI input graph violates its verified schema"
                : input_status.message);
    }

    const ProgramActionRequestId state_request =
        impl_->NextActionId();
    if (!state_request)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InternalFailure,
            "Program action request identity space is exhausted");
    }

    Impl::ActiveInvocation active;
    active.invocation = std::move(invocation);
    active.workset_epoch =
        active.invocation.state.expected_epoch;
    active.verified = std::move(verified.verified);
    active.cancellation = std::move(cancellation);
    active.events = std::move(events);
    active.state_request = state_request;
    impl_->active.emplace(std::move(active));

    ProgramActionRequest state;
    state.request_id = state_request;
    state.invocation_id = impl_->active->invocation.invocation_id;
    state.attempt_id = impl_->active->invocation.attempt_id;
    state.operation = ProgramHostOperation::PrepareInvocationState;
    state.expected_epoch =
        impl_->active->invocation.state.expected_epoch;
    state.input = UnitGraph();
    state.scope = kInvocationScope;
    state.state_request = impl_->active->invocation.state;
    state.state_already_prepared =
        request.state_already_prepared;
    state.prepared_baseline_sha256 =
        std::move(request.prepared_baseline_sha256);
    state.timing = ActionTimingClass::BoundedHostOperation;
    state.bounded_host_deadline =
        std::chrono::steady_clock::now() +
        impl_->config.bounded_host_operation_timeout;
    state.allowed_effects =
        AllowedEffects(impl_->active->invocation.execution);
    try
    {
        impl_->action_sink->Publish(std::move(state));
    }
    catch (...)
    {
        impl_->active.reset();
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InternalFailure,
            "Program action sink rejected state preparation");
    }
    return ProgramRuntimeSubmission::Accepted();
}

ProgramRuntimeSubmission ProgramRuntime::PrepareInvocationTemplate(
    InvocationTemplatePreparationRequest request,
    PreparedInvocationTemplateReceipt& receipt)
{
    receipt = {};
    if (impl_->shutdown || !impl_->initialized)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::ProgramRuntimeUnavailable,
            "Canonical ProgramRuntime is unavailable");
    }
    DecodeResult<ProgramInvocation> decoded =
        DecodeProgramInvocationV1(
            request.invocation_template.input_payload);
    if (!decoded)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            decoded.status.message.empty()
                ? "SPRI invocation template is invalid"
                : decoded.status.message);
    }
    ProgramInvocation invocation = std::move(*decoded.value);
    const auto outer_module =
        ModuleIdentityFromEnvelope(
            request.invocation_template.module);
    if (!outer_module ||
        invocation.invocation_id !=
            request.invocation_template.invocation_id ||
        invocation.attempt_id !=
            request.invocation_template.attempt_id ||
        invocation.module != *outer_module ||
        invocation.entrypoint !=
            request.invocation_template.entrypoint ||
        invocation.state.expected_session ||
        invocation.state.expected_epoch ||
        request.invocation_template.expected_workset_epoch)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "SPRI workset template identity or deferred state binding is invalid");
    }

    ProgramVerificationResult verified = impl_->verifier.Verify(
        invocation.module,
        impl_->config.compatibility);
    if (!verified.success)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            verified.diagnostics.empty()
                ? "Program invocation template module verification failed"
                : verified.diagnostics.front().message);
    }
    const ProgramEntrypoint* entrypoint = FindEntrypoint(
        *verified.verified->module,
        invocation.entrypoint);
    const ProgramValue* input = RootValue(invocation.input);
    const ProgramBudgets* maximum_budgets =
        entrypoint && entrypoint->narrowed_budgets
        ? &*entrypoint->narrowed_budgets
        : &verified.verified->module->budgets;
    if (!entrypoint || !input ||
        input->type != entrypoint->input_type ||
        invocation.dependencies !=
            verified.verified->dependency_lock ||
        !RuntimeProfileAccepted(
            invocation.runtime_profile,
            impl_->config,
            verified.verified->dependency_lock) ||
        !BudgetsWithin(invocation.limits, *maximum_budgets) ||
        !InvocationPolicyAccepted(invocation, *entrypoint) ||
        invocation.state.session_lineage.empty() ||
        !InvocationStateShapeAccepted(invocation.state))
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "SPRI template violates its verified entrypoint, dependency, state, or budget contract");
    }
    const ProgramValueArenaStatus input_status =
        ValidateProgramValueGraph(
            invocation.input,
            entrypoint->input_type,
            verified.verified->type_closure,
            {
                invocation.limits.maximum_values,
                invocation.limits.maximum_value_bytes,
            },
            {});
    if (!input_status)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            input_status.message.empty()
                ? "SPRI template input graph violates its verified schema"
                : input_status.message);
    }

    const std::string compatibility =
        ComputeProgramInvocationCompatibilityHashV1(invocation);
    if (compatibility.size() != 64 ||
        impl_->next_template_id == 0 ||
        impl_->next_template_id ==
            std::numeric_limits<std::uint64_t>::max())
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InternalFailure,
            "Program invocation template identity could not be created");
    }
    const PreparedInvocationTemplateId id(
        impl_->next_template_id++);
    impl_->prepared_templates.emplace(
        id.value(),
        Impl::PreparedTemplate{
            invocation,
            std::move(verified.verified),
            compatibility});
    receipt = {
        id,
        invocation.invocation_id,
        invocation.attempt_id,
        ModuleIdentityToEnvelope(invocation.module),
        invocation.entrypoint,
        compatibility,
        invocation.state.policy,
        invocation.limits.maximum_artifacts};
    return ProgramRuntimeSubmission::Accepted();
}

ProgramRuntimeSubmission ProgramRuntime::ReleaseInvocationTemplate(
    PreparedInvocationTemplateId template_id)
{
    if (!template_id ||
        impl_->prepared_templates.erase(template_id.value()) != 1)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Prepared invocation template is unknown");
    }
    return ProgramRuntimeSubmission::Accepted();
}

ProgramRuntimeSubmission ProgramRuntime::StartPreparedInvocation(
    PreparedInvocationStartRequest request,
    CancellationToken cancellation,
    std::shared_ptr<IProgramRuntimeEventSink> events)
{
    const auto found =
        impl_->prepared_templates.find(request.template_id.value());
    if (found == impl_->prepared_templates.end())
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Prepared invocation template is unknown");
    }
    if (!request.session_id || !request.workset_epoch ||
        request.baseline_sha256.size() != 64 ||
        request.baseline_lineage.empty() ||
        found->second.invocation.state.session_lineage !=
            request.baseline_lineage ||
        (found->second.invocation.state.policy ==
             InvocationStatePolicy::RestoreBaseline) !=
            request.state_already_prepared)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            "Prepared invocation binding does not match its exact program baseline");
    }

    ProgramInvocation invocation = found->second.invocation;
    invocation.state.expected_session = request.session_id;
    invocation.state.expected_epoch = request.workset_epoch;
    const EncodeResult encoded = EncodeProgramInvocationV1(invocation);
    if (!encoded)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InternalFailure,
            encoded.status.message.empty()
                ? "Prepared invocation could not be rebound"
                : encoded.status.message);
    }
    EncodedInvocationEnvelope envelope;
    envelope.invocation_id = invocation.invocation_id;
    envelope.attempt_id = invocation.attempt_id;
    envelope.module = ModuleIdentityToEnvelope(invocation.module);
    envelope.entrypoint = invocation.entrypoint;
    envelope.expected_workset_epoch = request.workset_epoch;
    envelope.input_payload = encoded.bytes;
    ProgramRuntimeSubmission started = StartInvocation(
        {
            request.command_sequence,
            std::move(envelope),
            request.state_already_prepared,
            request.baseline_sha256,
        },
        std::move(cancellation),
        std::move(events));
    if (started.accepted)
        impl_->prepared_templates.erase(found);
    return started;
}

ProgramRuntimeSubmission ProgramRuntime::RequestCancellation(
    InvocationId invocation_id)
{
    if (!impl_->active)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvocationNotActive,
            "Canonical ProgramRuntime has no active invocation");
    }
    if (impl_->active->invocation.invocation_id != invocation_id)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvocationMismatch,
            "Cancellation does not identify the active invocation");
    }
    if (impl_->active->stage ==
        Impl::ActiveStage::ExecutionFinished)
    {
        return ProgramRuntimeSubmission::ExecutionAlreadyFinished();
    }
    if (impl_->active->executor)
    {
        (void)impl_->active->executor->RequestCancellation(
            CancellationReason::ExternalRequest);
    }
    else if (impl_->active->pending_cancellation ==
             CancellationReason::None)
    {
        // State preparation is an actor-host action. Remember cancellation
        // until that bounded action completes so the executor cannot begin
        // ordinary work after cancellation won the actor ordering.
        impl_->active->pending_cancellation =
            CancellationReason::ExternalRequest;
    }
    return ProgramRuntimeSubmission::Accepted();
}

void ProgramRuntime::BindActionSink(
    std::shared_ptr<IProgramActionRequestSink> sink)
{
    if (impl_->active)
        return;
    impl_->action_sink = std::move(sink);
}

ProgramRuntimeSubmission ProgramRuntime::DeliverActionResolution(
    ProgramActionResolution completion)
{
    if (!impl_->active)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvocationNotActive,
            "No invocation is awaiting an action resolution");
    }
    if (completion.invocation_id !=
            impl_->active->invocation.invocation_id ||
        completion.attempt_id !=
            impl_->active->invocation.attempt_id)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvocationMismatch,
            "Action completion identifies another invocation");
    }

    if (impl_->active->stage == Impl::ActiveStage::PreparingState)
    {
        if (completion.request_id != impl_->active->state_request ||
            completion.operation !=
                ProgramHostOperation::PrepareInvocationState)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "State preparation completion does not match its request");
        }
        if (!completion.workset_epoch ||
            completion.workset_epoch !=
                impl_->active->invocation.state.expected_epoch)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::WorksetEpochMismatch,
                "State preparation completion has a stale origin epoch");
        }
        impl_->active->preparation_cleanup =
            completion.cleanup;
        impl_->active->preparation_disposition =
            completion.session_disposition;
        impl_->active->preparation_cleanup_receipts =
            std::move(completion.cleanup_receipts);
        if (completion.status !=
                ProgramActionResolutionStatus::Completed)
        {
            const ProgramInfrastructureStatus infrastructure =
                completion.status ==
                        ProgramActionResolutionStatus::Cancelled
                ? ProgramInfrastructureStatus::Cancelled
                : completion.status ==
                          ProgramActionResolutionStatus::TimedOut
                ? ProgramInfrastructureStatus::TimedOut
                : completion.status ==
                          ProgramActionResolutionStatus::StaleEpoch
                ? ProgramInfrastructureStatus::ContractFailed
                : ProgramInfrastructureStatus::Rejected;
            ProgramResult rejected{
                .invocation_id =
                    impl_->active->invocation.invocation_id,
                .attempt_id = impl_->active->invocation.attempt_id,
                .module = impl_->active->invocation.module,
                .entrypoint = impl_->active->invocation.entrypoint,
                .resolved_dependencies =
                    impl_->active->verified->dependency_lock,
                .infrastructure = infrastructure,
                .cleanup = completion.cleanup,
                .session_disposition =
                    completion.session_disposition,
                .diagnostics = {ProgramDiagnostic{
                    DiagnosticSeverity::Error,
                    completion.code.empty()
                        ? "state_preparation_failed"
                        : completion.code,
                    completion.message.empty()
                        ? "Invocation state preparation failed"
                        : completion.message,
                    {},
                    {}}},
                .provenance =
                    impl_->active->invocation.provenance,
            };
            impl_->FinishExecution(std::move(rejected));
            return ProgramRuntimeSubmission::Accepted();
        }
        auto executor = std::make_unique<ProgramExecutor>(
            [](const ExactDependencyIdentity& identity,
               std::span<const ProgramValueGraph> inputs,
               std::string& diagnostic) {
                return capabilities::InvokeSourceReducer(
                    identity,
                    inputs,
                    &diagnostic);
            });
        std::string diagnostic;
        if (!executor->Start(
                impl_->active->verified,
                impl_->active->invocation,
                impl_->active->cancellation,
                &diagnostic))
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                diagnostic.empty()
                    ? "ProgramExecutor could not start"
                    : diagnostic);
        }
        impl_->active->executor = std::move(executor);
        impl_->active->stage = Impl::ActiveStage::Executing;
        if (impl_->active->pending_cancellation !=
            CancellationReason::None)
        {
            (void)impl_->active->executor->RequestCancellation(
                impl_->active->pending_cancellation);
        }
        return ProgramRuntimeSubmission::Accepted();
    }

    if (!impl_->active->executor)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InternalFailure,
            "Active invocation has no ProgramExecutor");
    }
    std::string diagnostic;
    if (!impl_->active->executor->DeliverHostCompletion(
            std::move(completion),
            &diagnostic))
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            diagnostic.empty()
                ? "Action completion was rejected"
                : diagnostic);
    }
    impl_->active->invocation.state.expected_epoch =
        impl_->active->executor->snapshot().workset_epoch;
    return ProgramRuntimeSubmission::Accepted();
}

ProgramExecutionTakeResult ProgramRuntime::TakeFinishedExecution(
    InvocationId invocation_id,
    AttemptId attempt_id)
{
    if (!impl_->active)
    {
        return {false, std::nullopt, {
            WorkerRejectionCode::InvocationNotActive,
            "Canonical ProgramRuntime has no retained execution"}};
    }
    if (impl_->active->stage !=
            Impl::ActiveStage::ExecutionFinished)
    {
        return {};
    }
    if (!impl_->active->finished ||
        impl_->active->invocation.invocation_id != invocation_id ||
        impl_->active->invocation.attempt_id != attempt_id)
    {
        return {false, std::nullopt, {
            WorkerRejectionCode::InvocationMismatch,
            "Finished-execution take does not match the retained "
            "invocation attempt"}};
    }
    ProgramExecutionFinished finished =
        std::move(*impl_->active->finished);
    impl_->active.reset();
    return {true, std::move(finished), {}};
}

bool ProgramRuntime::Pump()
{
    if (!impl_->active)
    {
        return false;
    }

    if (impl_->active->stage == Impl::ActiveStage::PreparingState)
        return false;

    if (impl_->active->stage != Impl::ActiveStage::Executing ||
        !impl_->active->executor)
    {
        return false;
    }

    ProgramExecutorPumpResult pumped =
        impl_->active->executor->Pump(kProgramExecutorQuantum);
    if (pumped.emission)
    {
        if (impl_->active->events)
        {
            impl_->active->events->Publish(
                ProgramInvocationObservationEvent{
                    impl_->active->invocation.invocation_id,
                    impl_->active->invocation.attempt_id,
                    std::move(*pumped.emission)});
        }
        return pumped.runnable;
    }
    if (pumped.host_request)
    {
        const ProgramActionRequestId request_id =
            impl_->NextActionId();
        if (!request_id ||
            !impl_->active->executor->BindPendingAction(request_id))
        {
            (void)impl_->active->executor->RequestCancellation(
                CancellationReason::RuntimeFailure);
            return true;
        }

        ProgramActionRequest request;
        request.request_id = request_id;
        request.invocation_id =
            impl_->active->invocation.invocation_id;
        request.attempt_id =
            impl_->active->invocation.attempt_id;
        request.operation = pumped.host_request->operation;
        request.expected_epoch =
            impl_->active->invocation.state.expected_epoch;
        request.action = std::move(pumped.host_request->action);
        request.input = std::move(pumped.host_request->input);
        request.scope = pumped.host_request->scope;
        request.parent_scope = pumped.host_request->parent_scope;
        request.resource = pumped.host_request->resource;
        request.cleanup_only = pumped.host_request->cleanup_only;
        request.diagnostic_selector =
            std::move(pumped.host_request->diagnostic_selector);
        request.allowed_effects =
            AllowedEffects(impl_->active->invocation.execution);
        request.timing = ActionTimingClass::BoundedHostOperation;
        if (request.action)
        {
            const ActionDescriptor* descriptor =
                impl_->actions.ResolveAction(*request.action);
            if (descriptor)
            {
                request.timing = descriptor->timing;
                if (descriptor->timing ==
                        ActionTimingClass::BoundedHostOperation)
                {
                    request.bounded_host_deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            descriptor->
                                default_host_timeout_milliseconds);
                }
            }
        }
        impl_->action_sink->Publish(std::move(request));

        return false;
    }
    if (pumped.terminal)
    {
        impl_->FinishExecution(std::move(*pumped.terminal));
        return false;
    }
    return pumped.runnable;
}

std::optional<std::chrono::steady_clock::time_point>
ProgramRuntime::next_wake() const
{
    if (!impl_->active)
        return std::nullopt;
    if (impl_->active->stage == Impl::ActiveStage::PreparingState)
        return std::nullopt;
    if (impl_->active->stage != Impl::ActiveStage::Executing ||
        !impl_->active->executor)
    {
        return std::nullopt;
    }
    return impl_->active->executor->next_wake();
}

void ProgramRuntime::Shutdown() noexcept
{
    if (impl_->shutdown)
        return;
    impl_->shutdown = true;
    if (impl_->active)
    {
        if (impl_->active->stage !=
            Impl::ActiveStage::ExecutionFinished)
        {
            ProgramResult failed{
                .invocation_id =
                    impl_->active->invocation.invocation_id,
                .attempt_id =
                    impl_->active->invocation.attempt_id,
                .module = impl_->active->invocation.module,
                .entrypoint =
                    impl_->active->invocation.entrypoint,
                .resolved_dependencies =
                    impl_->active->verified->dependency_lock,
                .infrastructure =
                    ProgramInfrastructureStatus::Cancelled,
                .cleanup = ProgramCleanupStatus::Tainted,
                .session_disposition = SessionDisposition::Tainted,
                .diagnostics = {ProgramDiagnostic{
                    DiagnosticSeverity::Error,
                    "runtime_shutdown",
                    "ProgramRuntime shut down before actor cleanup "
                    "completed",
                    {},
                    {}}},
                .provenance =
                    impl_->active->invocation.provenance,
            };
            try
            {
                impl_->FinishExecution(std::move(failed));
            }
            catch (...)
            {
            }
        }
        impl_->active.reset();
    }
    impl_->prepared_templates.clear();
    impl_->action_sink.reset();
}

ProgramDefinitionStore& ProgramRuntime::definitions() noexcept
{
    return impl_->definitions;
}

TypeSchemaRegistry& ProgramRuntime::types() noexcept
{
    return impl_->types;
}

ActionRegistry& ProgramRuntime::actions() noexcept
{
    return impl_->actions;
}

CapabilityPackRegistry& ProgramRuntime::capability_packs() noexcept
{
    return impl_->packs;
}

std::unique_ptr<ProgramRuntime>
MakeSupportedSoaUsaProgramRuntime()
{
    return std::make_unique<ProgramRuntime>(
        ProgramRuntimeConfig{
            .compatibility =
                capabilities::SupportedSoaUsaCompatibility(),
            .runtime_profile = {
                .profile_id = "soa-usa-jit64-v1",
                .game_id =
                    std::string(capabilities::kSupportedGameId),
                .disc_identity =
                    std::string(capabilities::kSupportedGameId),
                .executable_identity =
                    std::string(
                        capabilities::
                            kSupportedExecutableIdentity),
                .backend = "jit64",
            }});
}

} // namespace savor::runtime::program
