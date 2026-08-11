#include "ProgramBaseline.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <array>
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

ProgramBaselineComponentResult FromSavestateFailure(
    const SavestateServiceResult& result,
    std::string fallback)
{
    return ProgramBaselineComponentResult::Failure(
        result.integrity == GuestIntegrity::Unknown
            ? WorkerRejectionCode::SessionTainted
            : WorkerRejectionCode::BackendFailure,
        result.message.empty() ? std::move(fallback) : result.message);
}

ProgramBaselineComponentResult FromSessionFailure(
    const SessionOperationReceipt& receipt,
    std::string fallback)
{
    return ProgramBaselineComponentResult::Failure(
        receipt.disposition == SessionDisposition::Tainted ||
                receipt.backend.integrity == BackendIntegrity::Unknown
            ? WorkerRejectionCode::SessionTainted
            : WorkerRejectionCode::BackendFailure,
        receipt.backend.message.empty()
            ? std::move(fallback)
            : receipt.backend.message);
}

ProgramBaselineComponentResult FromMovieFailure(
    const MovieServiceResult& result,
    std::string fallback)
{
    return ProgramBaselineComponentResult::Failure(
        result.integrity == GuestIntegrity::Unknown
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

std::string
TasMovieCheckpointSterilizationBaselineComponentProvider::canonical_id()
    const
{
    return std::string(
        kTasMovieCheckpointSterilizationBaselineComponentId);
}

std::uint32_t
TasMovieCheckpointSterilizationBaselineComponentProvider::revision()
    const noexcept
{
    return 1;
}

ProgramBaselineComponentResult
TasMovieCheckpointSterilizationBaselineComponentProvider::Stage(
    ProgramBaselineComponent& component)
{
    const auto expected =
        MakeTasMovieCheckpointSterilizationBaselineComponent();
    if (component != expected)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "TAS Movie checkpoint sterilization baseline component is not canonical");
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult
TasMovieCheckpointSterilizationBaselineComponentProvider::Activate(
    const ProgramBaselineComponent& component,
    EmulationSession& session,
    bool reset)
{
    if (component !=
        MakeTasMovieCheckpointSterilizationBaselineComponent())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "TAS Movie checkpoint sterilization baseline component changed after staging");
    }
    MovieService* movies = session.movie_service();
    if (!movies)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionUnavailable,
            "MovieService is unavailable during checkpoint sterilization");
    }
    if (reset && movies->activity() == MovieActivity::Inactive &&
        !movies->reservation())
    {
        return ProgramBaselineComponentResult::Success();
    }
    if (movies->activity() != MovieActivity::ReadOnlyPlayback ||
        !movies->reservation())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Checkpoint sterilization requires verified read-only baseline playback");
    }
    const MovieOperationReceipt stopped = movies->StopPlayback();
    if (!stopped.result.ok || movies->activity() != MovieActivity::Inactive ||
        movies->reservation())
    {
        return ProgramBaselineComponentResult::Failure(
            stopped.result.integrity == GuestIntegrity::Unknown
                ? WorkerRejectionCode::SessionTainted
                : WorkerRejectionCode::BackendFailure,
            stopped.result.message.empty()
                ? "Checkpoint sterilization could not detach movie playback"
                : stopped.result.message);
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponent
MakeTasMovieCheckpointSterilizationBaselineComponent()
{
    static constexpr std::array<std::uint8_t, 5> kBytes{
        'T', 'C', 'S', 'B', '1'};
    ProgramBaselineComponent component;
    component.canonical_id =
        kTasMovieCheckpointSterilizationBaselineComponentId;
    component.revision = 1;
    component.schema_id =
        kTasMovieCheckpointSterilizationBaselineSchemaId;
    component.policy = ProgramBaselineComponentPolicy::ResetForEveryItem;
    component.immutable_bytes.assign(kBytes.begin(), kBytes.end());
    component.content_sha256 = hash::sha256(
        component.immutable_bytes.data(), component.immutable_bytes.size());
    return component;
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
    const ProgramBaselineArtifact& artifact = definition.artifact;
    if (!artifact.compatibility.Complete() ||
        artifact.lineage.edge.empty() || artifact.lineage.producer.empty())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Artifact baseline requires complete compatibility and lineage");
    }
    const bool has_state = !artifact.state_path.empty();
    const bool has_movie = artifact.movie_path.has_value();
    if (artifact.kind == ProgramBaselineArtifactKind::Savestate)
    {
        if (!has_state || !CompleteSha256(artifact.state_sha256) ||
            has_movie != !artifact.movie_sha256.empty())
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Savestate baseline requires an exact state and complete optional DTM sidecar");
        }
        if (has_movie &&
            *artifact.movie_path !=
                SavestateDtmSidecarPath(artifact.state_path))
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Savestate baseline DTM is not its same-name sidecar");
        }
    }
    else if (artifact.kind == ProgramBaselineArtifactKind::ReadOnlyMovie)
    {
        if (!has_movie || !CompleteSha256(artifact.movie_sha256) ||
            (has_state != !artifact.state_sha256.empty()) ||
            (has_state && !CompleteSha256(artifact.state_sha256)))
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Read-only-movie baseline requires an exact DTM and complete optional startup savestate");
        }
    }
    else
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Artifact baseline kind is invalid");
    }
    try
    {
        if (has_state && hash::sha256_of_file(artifact.state_path.string()) !=
            artifact.state_sha256)
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Program baseline state hash does not match its artifact");
        }
        if (has_movie && hash::sha256_of_file(artifact.movie_path->string()) !=
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
            std::string("Program baseline artifact could not be staged: ") +
                ex.what());
    }
    if (!ComputeProgramBaselineKey(definition))
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Program baseline definition is not canonical");
    }
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Initialize(
    WorkerWorksetId workset_id,
    const ProgramBaselineDefinition& definition,
    bool multi_item,
    PreparedProgramBaselineReceipt& receipt_out)
{
    if (stopped_ || active_key_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Another workset baseline is active or the coordinator is stopped");
    }
    if (!session_.snapshot().open || session_.snapshot().workset_epoch)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::SessionUnavailable,
            "Program baseline requires an idle infrastructure session");
    }
    const SessionOperationReceipt opened =
        session_.OpenWorksetInitialization(workset_id);
    if (!opened.ok)
    {
        return FromSessionFailure(
            opened,
            "Workset initialization could not open");
    }
    active_workset_id_ = workset_id;
    ProgramBaselineComponentResult scope = OpenScope(workset_id);
    if (!scope.ok)
    {
        (void)session_.AbortWorksetInitialization(workset_id);
        active_workset_id_ = {};
        return scope;
    }

    active_definition_ = definition;
    active_key_ = ComputeProgramBaselineKey(definition);
    multi_item_ = multi_item;

    ProgramBaselineComponentResult source = PrepareSource(definition);
    if (!source.ok)
    {
        (void)Release();
        return source;
    }
    const bool established = definition.artifact.kind ==
        ProgramBaselineArtifactKind::Savestate;
    if (established)
    {
        ProgramBaselineComponentResult activated =
            components_->Activate(definition, session_, false);
        if (!activated.ok)
        {
            (void)Release();
            return activated;
        }
    }
    else if (!definition.components.empty())
    {
        (void)Release();
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Movie-established baselines cannot activate guest-dependent components before playback");
    }

    if (multi_item_ && established)
    {
        active_handle_ = session_.CaptureWorksetBaselineHandle();
        if (!active_handle_->result.ok)
        {
            const SavestateServiceResult failure = active_handle_->result;
            active_handle_.reset();
            (void)Release();
            return FromSavestateFailure(
                failure,
                "Workset-owned baseline capture failed");
        }
    }

    const SessionOperationReceipt committed =
        session_.CommitWorksetInitialization(workset_id);
    if (!committed.ok)
    {
        ProgramBaselineComponentResult failure = FromSessionFailure(
            committed,
            "Workset initialization could not establish authoritative execution evidence");
        (void)Release();
        return failure;
    }
    initialization_committed_ = true;
    const SessionSnapshot current = session_.snapshot();
    active_receipt_ = {
        active_key_,
        current.session_id,
        current.workset_epoch,
        definition.lineage,
        established};

    receipt_out = active_receipt_;
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult
WorksetStateCoordinator::RestoreForNextItem(
    PreparedProgramBaselineReceipt& receipt_out)
{
    if (stopped_ || !active_key_ || !multi_item_)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "No multi-item workset baseline is active");
    }
    if (session_.execution_snapshot())
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "Workset item reset still has authoritative execution evidence");
    }
    if (active_definition_.artifact.kind ==
        ProgramBaselineArtifactKind::ReadOnlyMovie)
    {
        ProgramBaselineComponentResult prepared =
            PrepareSource(active_definition_);
        if (!prepared.ok)
            return prepared;
        const SessionOperationReceipt committed =
            session_.CommitWorksetItemReset(active_workset_id_);
        if (!committed.ok)
        {
            return FromSessionFailure(
                committed,
                "Movie workset item reset could not establish authoritative execution evidence");
        }
        const SessionSnapshot current = session_.snapshot();
        active_receipt_ = {
            active_key_, current.session_id, current.workset_epoch,
            active_definition_.lineage, false};
        receipt_out = active_receipt_;
        return ProgramBaselineComponentResult::Success();
    }
    if (!active_handle_ || !active_handle_->handle)
    {
        return ProgramBaselineComponentResult::Failure(
            WorkerRejectionCode::InvalidState,
            "No workset-owned savestate handle is active");
    }
    const SavestateRestoreReceipt restored =
        session_.RestoreWorksetBaselineHandle(active_handle_->handle);
    if (!restored.result.ok)
    {
        return FromSavestateFailure(
            restored.result,
            "Workset baseline restore failed");
    }
    ProgramBaselineComponentResult reset =
        components_->Activate(active_definition_, session_, true);
    if (!reset.ok)
        return reset;
    const SessionOperationReceipt committed =
        session_.CommitWorksetItemReset(active_workset_id_);
    if (!committed.ok)
    {
        return FromSessionFailure(
            committed,
            "Savestate workset item reset could not establish authoritative execution evidence");
    }
    active_receipt_ = {
        active_key_,
        session_.snapshot().session_id,
        restored.workset_epoch,
        active_definition_.lineage,
        true};
    receipt_out = active_receipt_;
    return ProgramBaselineComponentResult::Success();
}

ProgramBaselineComponentResult WorksetStateCoordinator::Release()
{
    ProgramBaselineComponentResult handle_result =
        ProgramBaselineComponentResult::Success();
    if (active_handle_ && active_handle_->handle)
    {
        const SavestateServiceResult released =
            session_.ReleaseWorksetBaselineHandle(active_handle_->handle);
        if (!released.ok)
            handle_result = FromSavestateFailure(
                released, "Workset-owned baseline release failed");
    }
    active_handle_.reset();
    ProgramBaselineComponentResult staged_result =
        ProgramBaselineComponentResult::Success();
    if (staged_source_root_)
    {
        std::error_code error;
        std::filesystem::remove_all(*staged_source_root_, error);
        if (error)
        {
            staged_result = ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::BackendFailure,
                "Worker-private baseline staging cleanup failed: " +
                    error.message());
        }
        staged_source_root_.reset();
    }
    ProgramBaselineComponentResult scope_result = CloseScope();
    ProgramBaselineComponentResult end_result =
        ProgramBaselineComponentResult::Success();
    if (active_workset_id_)
    {
        const SessionOperationReceipt ended = initialization_committed_
            ? session_.EndWorkset(active_workset_id_)
            : session_.AbortWorksetInitialization(active_workset_id_);
        if (!ended.ok)
        {
            end_result = ProgramBaselineComponentResult::Failure(
                ended.disposition == SessionDisposition::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::BackendFailure,
                ended.backend.message.empty()
                    ? "Workset runtime cleanup failed"
                    : ended.backend.message);
        }
    }
    active_workset_id_ = {};
    active_definition_ = {};
    active_key_ = {};
    active_receipt_ = {};
    multi_item_ = false;
    initialization_committed_ = false;
    if (!handle_result.ok)
        return handle_result;
    if (!staged_result.ok)
        return staged_result;
    if (!scope_result.ok)
        return scope_result;
    return end_result;
}

ProgramBaselineComponentResult WorksetStateCoordinator::Shutdown()
{
    if (stopped_)
        return ProgramBaselineComponentResult::Success();
    ProgramBaselineComponentResult released = Release();
    if (!released.ok)
        return released;
    stopped_ = true;
    return ProgramBaselineComponentResult::Success();
}

WorksetBaselineSnapshot WorksetStateCoordinator::snapshot() const noexcept
{
    return {
        static_cast<bool>(active_key_),
        multi_item_,
        active_key_,
        active_receipt_,
        workset_scope_};
}

ProgramBaselineComponentResult WorksetStateCoordinator::PrepareSource(
    const ProgramBaselineDefinition& definition)
{
    if (definition.artifact.kind ==
        ProgramBaselineArtifactKind::ReadOnlyMovie)
    {
        MovieService* movies = session_.movie_service();
        if (!movies || !definition.artifact.movie_path)
        {
            return ProgramBaselineComponentResult::Failure(
                movies
                    ? WorkerRejectionCode::InvalidArgument
                    : WorkerRejectionCode::Unsupported,
                movies
                    ? "Read-only-movie baseline has no exact DTM artifact"
                    : "MovieService is unavailable during workset initialization");
        }
        const MovieOperationReceipt prepared =
            movies->PrepareReadOnlyPlayback({
                .dtm_path = *definition.artifact.movie_path});
        if (!prepared.result.ok || !prepared.preparation)
        {
            return FromMovieFailure(
                prepared.result,
                "Read-only-movie baseline preparation failed");
        }
        if (prepared.dtm_sha256 != definition.artifact.movie_sha256)
        {
            const MovieOperationReceipt abandoned =
                movies->AbandonPreparedReadOnlyPlayback(
                    prepared.preparation);
            if (!abandoned.result.ok)
            {
                return FromMovieFailure(
                    abandoned.result,
                    "Prepared movie hash mismatch could not be rolled back");
            }
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Prepared read-only movie does not match the baseline hash");
        }
        return ProgramBaselineComponentResult::Success();
    }
    const ProgramBaselineArtifact& artifact = definition.artifact;
    std::filesystem::path state_path = artifact.state_path;
    std::optional<std::filesystem::path> movie_path = artifact.movie_path;
    if (artifact.movie_path)
    {
        try
        {
            const SessionSnapshot current = session_.snapshot();
            const std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                "savor-workset-baselines" /
                (std::to_string(current.session_id.value()) + "-" +
                 std::to_string(current.workset_epoch.value()) + "-" +
                 active_key_.sha256);
            std::error_code error;
            std::filesystem::create_directories(root, error);
            if (error)
            {
                return ProgramBaselineComponentResult::Failure(
                    WorkerRejectionCode::BackendFailure,
                    "Worker-private baseline staging directory failed: " +
                        error.message());
            }
            state_path = root / "baseline.sav";
            movie_path = std::filesystem::path(
                state_path.string() + ".dtm");
            std::filesystem::copy_file(
                artifact.state_path, state_path,
                std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(
                *artifact.movie_path, *movie_path,
                std::filesystem::copy_options::overwrite_existing);
            staged_source_root_ = root;
        }
        catch (const std::exception& exception)
        {
            return ProgramBaselineComponentResult::Failure(
                WorkerRejectionCode::BackendFailure,
                std::string("Worker-private baseline staging failed: ") +
                    exception.what());
        }
    }
    SavestateImportRequest request;
    request.path = state_path;
    request.expected_sha256 = artifact.state_sha256;
    request.compatibility = artifact.compatibility;
    request.movie_mode = artifact.movie_path
        ? ExternalMovieImportMode::ReadOnlyPlayback
        : ExternalMovieImportMode::NoMovie;
    request.dtm_path = movie_path;
    request.expected_dtm_sha256 = artifact.movie_sha256;
    request.lineage = artifact.lineage;
    const SavestateFileArtifactReceipt imported =
        session_.ImportWorksetBaselineArtifact(request);
    if (!imported.result.ok)
    {
        return FromSavestateFailure(
            imported.result,
            "Program baseline artifact import failed");
    }
    const SavestateRestoreReceipt restored =
        session_.RestoreWorksetBaselineArtifact(imported.artifact);
    const SavestateServiceResult released =
        session_.ReleaseWorksetBaselineArtifact(imported.artifact);
    if (!released.ok)
    {
        return FromSavestateFailure(
            released,
            restored.result.ok
                ? "Program baseline imported artifact release failed"
                : "Program baseline restore failed and its imported artifact could not be released");
    }
    if (!restored.result.ok)
    {
        return FromSavestateFailure(
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

} // namespace savor::runtime
