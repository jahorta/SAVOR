#include "ProgramBaseline.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace savor::runtime {
namespace {

bool CompleteSha256(std::string_view value)
{
    if (value.size() != 64)
        return false;
    return std::all_of(
        value.begin(),
        value.end(),
        [](char ch)
        {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

ProgramBaselineComponentResult FromStateFailure(
    const StateServiceResult& result,
    std::string fallback)
{
    return ProgramBaselineComponentResult::Failure(
        result.integrity == StateIntegrity::Unknown
            ? WorkerRejectionCode::SessionTainted
            : WorkerRejectionCode::BackendFailure,
        result.message.empty() ? std::move(fallback) : result.message);
}

} // namespace

ProgramBaselineComponentResult ProgramBaselineComponentRegistry::Register(
    std::shared_ptr<IProgramBaselineComponentProvider> provider)
{
    std::lock_guard lock(mutex_);
    if (frozen_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Baseline-component registry is frozen");
    }
    if (!provider || provider->canonical_id().empty() ||
        provider->revision() == 0)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Baseline-component provider identity is invalid");
    }
    const std::string key =
        Key(provider->canonical_id(), provider->revision());
    if (providers_.contains(key))
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Baseline-component provider identity is already registered");
    }
    providers_.emplace(key, std::move(provider));
    return ProgramBaselineComponentResult::Success();
}

void ProgramBaselineComponentRegistry::Freeze() noexcept
{
    std::lock_guard lock(mutex_);
    frozen_ = true;
}

bool ProgramBaselineComponentRegistry::frozen() const noexcept
{
    std::lock_guard lock(mutex_);
    return frozen_;
}

ProgramBaselineComponentResult ProgramBaselineComponentRegistry::Stage(
    ProgramBaselineDefinition& definition) const
{
    std::lock_guard lock(mutex_);
    for (ProgramBaselineComponent& component : definition.components)
    {
        const auto found =
            providers_.find(Key(component.canonical_id, component.revision));
        if (found == providers_.end())
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::Unsupported,
                "Program baseline requires an unregistered component provider");
        }
        ProgramBaselineComponentResult staged =
            found->second->Stage(component);
        if (!staged.ok)
            return staged;
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult ProgramBaselineComponentRegistry::Activate(
    const ProgramBaselineDefinition& definition,
    EmulationSession& session,
    bool reset) const
{
    std::lock_guard lock(mutex_);
    for (const ProgramBaselineComponent& component : definition.components)
    {
        const auto found =
            providers_.find(Key(component.canonical_id, component.revision));
        if (found == providers_.end())
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::Unsupported,
                "Program baseline component provider disappeared");
        }
        ProgramBaselineComponentResult activated =
            found->second->Activate(component, session, reset);
        if (!activated.ok)
            return activated;
    }
    return ProgramBaselineComponentResult::Success();
}

std::string ProgramBaselineComponentRegistry::Key(
    std::string_view canonical_id,
    std::uint32_t revision)
{
    return std::string(canonical_id) + "#" + std::to_string(revision);
}

InlineProgramBaselineComponentProvider::
    InlineProgramBaselineComponentProvider(
        std::string canonical_id,
        std::uint32_t revision)
    : canonical_id_(std::move(canonical_id)),
      revision_(revision)
{
}

std::string InlineProgramBaselineComponentProvider::canonical_id() const
{
    return canonical_id_;
}

std::uint32_t
InlineProgramBaselineComponentProvider::revision() const noexcept
{
    return revision_;
}

ProgramBaselineComponentResult
InlineProgramBaselineComponentProvider::Stage(
    ProgramBaselineComponent& component)
{
    if (component.canonical_id != canonical_id_ ||
        component.revision != revision_ || component.schema_id.empty() ||
        component.immutable_bytes.empty())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Inline baseline component is incomplete");
    }
    const std::string actual = hash::sha256(
        component.immutable_bytes.data(),
        component.immutable_bytes.size());
    if (!CompleteSha256(component.content_sha256) ||
        component.content_sha256 != actual)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Inline baseline component hash does not match its bytes");
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult
InlineProgramBaselineComponentProvider::Activate(
    const ProgramBaselineComponent&,
    EmulationSession&,
    bool)
{
    // Inline immutable/derived data is made available through the invocation
    // template. It contributes to exact baseline identity but owns no Dolphin
    // authority.
    return ProgramBaselineComponentResult::Success();
}

WorksetStateCoordinator::WorksetStateCoordinator(
    EmulationSession& session,
    WorkerWorksetLimits limits,
    std::shared_ptr<ProgramBaselineComponentRegistry> components)
    : session_(session),
      limits_(limits),
      components_(std::move(components))
{
    if (!components_)
        components_ = std::make_shared<ProgramBaselineComponentRegistry>();
}

WorksetStateCoordinator::~WorksetStateCoordinator()
{
    (void)Shutdown();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Stage(
    ProgramBaselineDefinition& definition) const
{
    if (stopped_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::RuntimeStopping,
            "Workset state coordinator is stopped");
    }
    ProgramBaselineComponentResult staged =
        components_->Stage(definition);
    if (!staged.ok)
        return staged;
    const bool artifact_source =
        definition.state_kind == ProgramBaselineStateKind::Artifact;
    const bool current_source =
        definition.state_kind ==
        ProgramBaselineStateKind::CurrentSession;
    if (artifact_source != definition.artifact.has_value() ||
        current_source != definition.current_session.has_value())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Program baseline kind does not match its exact state source");
    }
    if (current_source &&
        !static_cast<bool>(*definition.current_session))
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Current-session baseline requires exact clean-idle session and epoch evidence");
    }
    if (artifact_source)
    {
        if (!definition.artifact ||
            definition.artifact->state_path.empty() ||
            !CompleteSha256(
                definition.artifact->state_sha256) ||
            !definition.artifact->compatibility.Complete())
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Artifact baseline requires an exact immutable state");
        }
        const ProgramBaselineArtifact& artifact =
            *definition.artifact;
        const bool no_movie =
            artifact.movie_mode ==
            ExternalMovieImportMode::NoMovie;
        const bool read_only =
            artifact.movie_mode ==
            ExternalMovieImportMode::ReadOnlyPlayback;
        const std::filesystem::path expected_movie =
            std::filesystem::path(
                artifact.state_path.string() + ".dtm");
        if ((!no_movie && !read_only) ||
            (no_movie &&
             (artifact.movie_path ||
              !artifact.movie_sha256.empty())) ||
            (read_only &&
             (!artifact.movie_path ||
              artifact.movie_path != expected_movie ||
              !CompleteSha256(artifact.movie_sha256))))
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Program baseline movie policy does not match its exact sidecar");
        }
        try
        {
            if (hash::sha256_of_file(
                    definition.artifact->state_path.string()) !=
                definition.artifact->state_sha256)
            {
                return ProgramBaselineComponentResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Program baseline state hash does not match its artifact");
            }
            if (artifact.movie_path &&
                hash::sha256_of_file(
                    artifact.movie_path->string()) !=
                    artifact.movie_sha256)
            {
                return ProgramBaselineComponentResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Program baseline movie hash does not match its artifact");
            }
        }
        catch (const std::exception& ex)
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                std::string(
                    "Program baseline artifact could not be staged: ") +
                    ex.what());
        }
    }
    if (!ComputeProgramBaselineKey(definition))
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Program baseline definition is not canonical");
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Prepare(
    WorkerWorksetId workset_id,
    const ProgramBaselineDefinition& definition,
    bool reusable,
    PreparedProgramBaselineReceipt& receipt_out)
{
    if (stopped_ || active_key_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Another workset baseline is active or the coordinator is stopped");
    }
    if (!session_.snapshot().open || !session_.state_service())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionUnavailable,
            "Program baseline requires an open state service");
    }
    ProgramBaselineComponentResult cache_ready = EnsureCache();
    if (!cache_ready.ok)
        return cache_ready;

    ProgramBaselineComponentResult scope = OpenScope(workset_id);
    if (!scope.ok)
        return scope;

    active_definition_ = definition;
    active_key_ = ComputeProgramBaselineKey(definition);
    reusable_ = reusable;

    if (reusable_)
    {
        const auto known =
            baseline_cache_keys_.find(active_key_.sha256);
        if (known != baseline_cache_keys_.end())
        {
            active_lease_ = cache_->Acquire(known->second);
            if (active_lease_)
            {
                const StateOperationReceipt restored =
                    cache_->Restore(*active_lease_);
                if (!restored.result.ok)
                {
                    if (restored.result.integrity ==
                        StateIntegrity::Unknown)
                    {
                        const auto failure = FromStateFailure(
                            restored.result,
                            "Cached program baseline restore failed");
                        (void)Release();
                        return failure;
                    }
                    active_lease_.reset();
                    const StateServiceResult removed =
                        cache_->Remove(known->second);
                    baseline_cache_keys_.erase(known);
                    if (!removed.ok)
                    {
                        const auto failure = FromStateFailure(
                            removed,
                            "Failed cached baseline could not be released");
                        (void)Release();
                        return failure;
                    }
                    // A preserved-integrity cache failure is a performance
                    // miss. Reconstruct the exact source below.
                }
                else
                {
                    ProgramBaselineComponentResult activated =
                        components_->Activate(
                            active_definition_,
                            session_,
                            true);
                    if (!activated.ok)
                    {
                        (void)Release();
                        return activated;
                    }
                    active_receipt_ = {
                        active_key_,
                        session_.snapshot().session_id,
                        restored.resulting_epoch,
                        definition.lineage,
                        true};
                    receipt_out = active_receipt_;
                    return ProgramBaselineComponentResult::Success();
                }
            }
            else
            {
                // A stale index cannot turn an ordinary cache miss into a
                // correctness failure.
                baseline_cache_keys_.erase(known);
            }
        }
    }

    ProgramBaselineComponentResult source = PrepareSource(definition);
    if (!source.ok)
    {
        (void)Release();
        return source;
    }
    ProgramBaselineComponentResult activated =
        components_->Activate(definition, session_, false);
    if (!activated.ok)
    {
        (void)Release();
        return activated;
    }

    const SessionSnapshot current = session_.snapshot();
    active_receipt_ = {
        active_key_,
        current.session_id,
        current.state_epoch,
        definition.lineage,
        false};

    if (reusable_)
    {
        const StateHandleReceipt captured =
            session_.CaptureStateHandle();
        if (!captured.result.ok)
        {
            (void)Release();
            return FromStateFailure(
                captured.result,
                "Reusable program baseline capture failed");
        }
        const StateCacheKey cache_key =
            MakeCacheKey(definition, captured);
        StateServiceResult cache_error;
        active_lease_ = cache_->InsertAndAcquire(
            cache_key,
            captured,
            &cache_error);
        if (!active_lease_)
        {
            (void)Release();
            return FromStateFailure(
                cache_error,
                "Reusable program baseline cache insertion failed");
        }
        baseline_cache_keys_[active_key_.sha256] = cache_key;
    }

    receipt_out = active_receipt_;
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult
WorksetStateCoordinator::RestoreForNextItem(
    PreparedProgramBaselineReceipt& receipt_out)
{
    if (stopped_ || !active_key_ || !reusable_ || !active_lease_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "No reusable workset baseline is active");
    }
    const StateOperationReceipt restored =
        cache_->Restore(*active_lease_);
    if (!restored.result.ok)
    {
        return FromStateFailure(
            restored.result,
            "Workset baseline restore failed");
    }
    ProgramBaselineComponentResult reset =
        components_->Activate(active_definition_, session_, true);
    if (!reset.ok)
        return reset;
    active_receipt_ = {
        active_key_,
        session_.snapshot().session_id,
        restored.resulting_epoch,
        active_definition_.lineage,
        true};
    receipt_out = active_receipt_;
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Release()
{
    active_lease_.reset();
    active_definition_ = {};
    active_key_ = {};
    active_receipt_ = {};
    reusable_ = false;
    return CloseScope();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Shutdown()
{
    if (stopped_)
        return ProgramBaselineComponentResult::Success();
    ProgramBaselineComponentResult released = Release();
    if (!released.ok)
        return released;
    if (cache_)
    {
        const StateServiceResult invalidated = cache_->InvalidateAll();
        if (!invalidated.ok)
            return FromStateFailure(invalidated, "State-cache shutdown failed");
    }
    stopped_ = true;
    return ProgramBaselineComponentResult::Success();
}

WorksetBaselineSnapshot WorksetStateCoordinator::snapshot() const noexcept
{
    return {
        static_cast<bool>(active_key_),
        reusable_,
        active_key_,
        active_receipt_,
        workset_scope_};
}

SessionStateCacheSnapshot
WorksetStateCoordinator::cache_snapshot() const noexcept
{
    return cache_ ? cache_->snapshot() : SessionStateCacheSnapshot{};
}

ProgramBaselineComponentResult WorksetStateCoordinator::PrepareSource(
    const ProgramBaselineDefinition& definition)
{
    switch (definition.state_kind)
    {
    case ProgramBaselineStateKind::Boot: {
        const SessionOperationReceipt rebooted = session_.Reboot();
        if (!rebooted.ok)
        {
            return ProgramBaselineComponentResult::Failure(
                rebooted.disposition == SessionDisposition::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::BackendFailure,
                rebooted.backend.message.empty()
                    ? "Program baseline reboot failed"
                    : rebooted.backend.message);
        }
        return ProgramBaselineComponentResult::Success();
    }
    case ProgramBaselineStateKind::CurrentSession:
    {
        if (!definition.current_session)
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Current-session baseline guard is missing");
        }
        const SessionSnapshot current = session_.snapshot();
        const ExecutionSnapshot execution =
            session_.execution_snapshot();
        const bool clean =
            current.disposition == SessionDisposition::Clean ||
            current.disposition ==
                SessionDisposition::CleanWithDiagnostics;
        const bool idle =
            current.open &&
            current.core_state == BackendCoreState::Paused &&
            execution.activity == ExecutionActivity::IdlePaused &&
            !execution.active_operation &&
            execution.interruption_depth == 0;
        if (current.session_id !=
                definition.current_session->session_id ||
            current.state_epoch !=
                definition.current_session->state_epoch)
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::StateEpochMismatch,
                "Current-session baseline identifies another session or epoch");
        }
        if (!clean || !idle ||
            !definition.current_session->require_clean_idle)
        {
            return ProgramBaselineComponentResult::Failure(
                current.disposition == SessionDisposition::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::InvalidState,
                "Current-session baseline requires a clean, idle, paused session");
        }
        return ProgramBaselineComponentResult::Success();
    }
    case ProgramBaselineStateKind::Artifact:
        break;
    }

    if (!definition.artifact)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Artifact program baseline has no artifact");
    }
    const ProgramBaselineArtifact& artifact = *definition.artifact;
    StateFileImportRequest request;
    request.path = artifact.state_path;
    request.expected_sha256 = artifact.state_sha256;
    request.compatibility = artifact.compatibility;
    request.movie_mode = artifact.movie_mode;
    request.dtm_path = artifact.movie_path;
    request.expected_dtm_sha256 = artifact.movie_sha256;
    request.lineage = artifact.lineage;
    const StateFileArtifactReceipt imported =
        session_.ImportStateArtifact(request);
    if (!imported.result.ok)
    {
        return FromStateFailure(
            imported.result,
            "Program baseline artifact import failed");
    }
    const StateOperationReceipt restored =
        session_.RestoreStateArtifact(imported.artifact);
    StateService* states = session_.state_service();
    const StateServiceResult released = states
        ? states->ReleaseFileArtifact(imported.artifact)
        : StateServiceResult::Failure(
              StateServiceErrorCode::InvalidState,
              "State service disappeared during baseline preparation");
    if (!released.ok)
    {
        return FromStateFailure(
            released,
            restored.result.ok
                ? "Program baseline imported artifact release failed"
                : "Program baseline restore failed and its imported artifact could not be released");
    }
    if (!restored.result.ok)
    {
        return FromStateFailure(
            restored.result,
            "Program baseline artifact restore failed");
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::OpenScope(
    WorkerWorksetId workset_id)
{
    SessionResourceLedger* ledger = session_.resources();
    if (!ledger || !workset_id)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Workset resource ledger or identity is unavailable");
    }
    const ResourceLedgerSnapshot snapshot = ledger->snapshot();
    const ResourceScopeResult opened = ledger->OpenSyntheticScope(
        snapshot.session_root,
        ResourceOwnerId(workset_id.value()),
        "worker workset " + std::to_string(workset_id.value()));
    if (!opened.success)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            opened.error.message.empty()
                ? "Workset resource scope could not be opened"
                : opened.error.message);
    }
    workset_scope_ = opened.scope.id;
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::CloseScope()
{
    if (!workset_scope_)
        return ProgramBaselineComponentResult::Success();
    SessionResourceLedger* ledger = session_.resources();
    program::SessionResourceBindingTable* bindings =
        session_.resource_bindings();
    if (!ledger || !bindings)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionTainted,
            "Workset resource scope cleanup authority disappeared");
    }
    const ResourceUnwindResult closed =
        ledger->CloseScope(workset_scope_, *bindings);
    workset_scope_ = {};
    if (!closed.completed() ||
        closed.disposition == ResourceCleanupDisposition::TaintRequired)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionTainted,
            closed.error.message.empty()
                ? "Workset resource scope cleanup failed"
                : closed.error.message);
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::EnsureCache()
{
    if (cache_)
        return ProgramBaselineComponentResult::Success();
    StateService* states = session_.state_service();
    if (!states)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionUnavailable,
            "State cache requires an open StateService");
    }
    cache_ = std::make_unique<SessionStateCache>(
        *states,
        limits_.maximum_state_cache_entries,
        limits_.maximum_state_cache_bytes);
    return ProgramBaselineComponentResult::Success();
}

StateCacheKey WorksetStateCoordinator::MakeCacheKey(
    const ProgramBaselineDefinition& definition,
    const StateHandleReceipt& captured) const
{
    StateCacheKey key;
    key.baseline = active_key_;
    key.state_sha256 = captured.sha256;
    key.lineage = definition.lineage;
    key.compatibility = captured.compatibility;
    if (captured.movie)
        key.movie_continuation_sha256 = captured.movie->dtm_sha256;
    if (const StateService* states = session_.state_service())
        key.session_generation = states->session_generation();
    return key;
}

} // namespace savor::runtime
