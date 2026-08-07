#include "WorksetStager.h"

#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include <exception>
#include <filesystem>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] WorksetStagerResult Invalid(
    std::string message)
{
    return WorksetStagerResult::Failure(
        WorksetStagerErrorCode::InvalidArgument,
        std::move(message));
}

} // namespace

WorksetStager::WorksetStager(
    WorkerWorksetLimits limits,
    std::shared_ptr<ProgramBaselineComponentRegistry> components,
    std::shared_ptr<IWorksetStagerNotifier> notifier)
    : limits_(std::move(limits)),
      components_(std::move(components)),
      notifier_(std::move(notifier))
{
    if (!components_)
    {
        components_ =
            std::make_shared<ProgramBaselineComponentRegistry>();
    }
    thread_ = std::thread([this] { WorkerMain(); });
}

WorksetStager::~WorksetStager()
{
    Shutdown();
}

WorksetStagingSubmission WorksetStager::Submit(
    WorkerWorksetDefinition definition)
{
    const WorksetValidationResult validated =
        ValidateWorkerWorksetDefinition(definition, limits_);
    if (!validated.ok)
    {
        return {
            Invalid(
                validated.error.message.empty()
                    ? "Worker workset failed structural validation"
                    : validated.error.message),
            {}};
    }

    std::lock_guard lock(mutex_);
    if (!accepting_)
    {
        return {
            WorksetStagerResult::Failure(
                WorksetStagerErrorCode::NotAccepting,
                "Workset stager is shutting down"),
            {}};
    }
    if (queued_ || running_ || !completions_.empty())
    {
        return {
            WorksetStagerResult::Failure(
                WorksetStagerErrorCode::CapacityExceeded,
                "The one staged-successor slot is occupied"),
            {}};
    }
    if (next_staging_id_ == 0)
    {
        return {
            WorksetStagerResult::Failure(
                WorksetStagerErrorCode::SequenceExhausted,
                "Workset staging identity is exhausted"),
            {}};
    }
    const WorksetStagingId id(next_staging_id_++);
    queued_.emplace(Job{id, std::move(definition)});
    wake_.notify_one();
    return {WorksetStagerResult::Success(), id};
}

std::vector<WorksetStagingCompletion>
WorksetStager::DrainResults()
{
    std::lock_guard lock(mutex_);
    std::vector<WorksetStagingCompletion> drained;
    drained.reserve(completions_.size());
    while (!completions_.empty())
    {
        drained.push_back(std::move(completions_.front()));
        completions_.pop_front();
    }
    return drained;
}

void WorksetStager::Shutdown() noexcept
{
    {
        std::unique_lock lock(mutex_);
        if (joined_)
            return;
        if (joining_)
        {
            stopped_.wait(lock, [&] { return joined_; });
            return;
        }
        accepting_ = false;
        shutdown_ = true;
        joining_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    {
        std::lock_guard lock(mutex_);
        joining_ = false;
        joined_ = true;
    }
    stopped_.notify_all();
}

WorksetStagerSnapshot WorksetStager::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        accepting_,
        shutdown_,
        queued_.has_value(),
        running_,
        !completions_.empty()};
}

void WorksetStager::WorkerMain() noexcept
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(
                lock,
                [&] { return shutdown_ || queued_.has_value(); });
            if (!queued_)
            {
                if (shutdown_)
                    return;
                continue;
            }
            job = std::move(*queued_);
            queued_.reset();
            running_ = true;
        }

        WorksetStagingCompletion completion =
            Process(std::move(job));
        std::shared_ptr<IWorksetStagerNotifier> notifier;
        {
            std::lock_guard lock(mutex_);
            running_ = false;
            completions_.push_back(std::move(completion));
            notifier = notifier_;
        }
        if (notifier)
            notifier->NotifyWorksetStagingCompletion();
    }
}

WorksetStagingCompletion WorksetStager::Process(
    Job job) const noexcept
{
    WorksetStagingCompletion completion;
    completion.staging_id = job.id;
    completion.workset_id = job.definition.workset_id;
    try
    {
        ProgramBaselineComponentResult components =
            components_->Stage(job.definition.baseline);
        if (!components.ok)
        {
            completion.result = WorksetStagerResult::Failure(
                WorksetStagerErrorCode::ComponentFailure,
                components.error.message.empty()
                    ? "Program baseline component staging failed"
                    : components.error.message);
            return completion;
        }

        if (job.definition.phase_invocation.program.canonical_id ==
            "savor.full_phase.tas_movie_validation")
        {
            const ProgramBaselineArtifact& baseline =
                job.definition.baseline.artifact;
            if (baseline.kind != ProgramBaselineArtifactKind::ReadOnlyMovie ||
                !baseline.movie_path)
            {
                completion.result = Invalid(
                    "TAS Movie requires a read-only-movie artifact baseline");
                return completion;
            }
            for (const WorksetItemTemplate& item : job.definition.items)
            {
                tasmovie::TasMovieValidationRequestV1 request;
                std::string diagnostic;
                if (!tasmovie::DecodeTasMovieValidationExecutionInputV1(
                        item.execution.input_payload, request, &diagnostic) ||
                    std::filesystem::path(request.dtm_path) !=
                        *baseline.movie_path ||
                    request.startup_savestate_path.has_value() !=
                        !baseline.state_path.empty() ||
                    (request.startup_savestate_path &&
                     std::filesystem::path(*request.startup_savestate_path) !=
                         baseline.state_path))
                {
                    completion.result = Invalid(
                        diagnostic.empty()
                            ? "TAS Movie scalar input disagrees with its exact DTM/startup-savestate baseline"
                            : std::move(diagnostic));
                    return completion;
                }
            }
        }

        const ProgramBaselineKey baseline =
            ComputeProgramBaselineKey(job.definition.baseline);
        if (!baseline ||
            baseline != job.definition.execution_key.baseline)
        {
            completion.result = Invalid(
                "Host-staged baseline identity changed or is not canonical");
            return completion;
        }

        std::optional<HostStagedArtifactEvidence> evidence;
        const ProgramBaselineArtifact& artifact =
            job.definition.baseline.artifact;
        HostStagedArtifactEvidence observed;
        if (!artifact.state_path.empty())
        {
            observed.state_sha256 =
                hash::sha256_of_file(artifact.state_path.string());
            observed.state_bytes = static_cast<std::size_t>(
                std::filesystem::file_size(artifact.state_path));
            if (observed.state_sha256 != artifact.state_sha256)
            {
                completion.result = WorksetStagerResult::Failure(
                    WorksetStagerErrorCode::ArtifactFailure,
                    "Program baseline state hash does not match");
                return completion;
            }
        }
        if (artifact.movie_path)
        {
            observed.movie_sha256 =
                hash::sha256_of_file(artifact.movie_path->string());
            observed.movie_bytes = static_cast<std::size_t>(
                std::filesystem::file_size(*artifact.movie_path));
            if (observed.movie_sha256 != artifact.movie_sha256)
            {
                completion.result = WorksetStagerResult::Failure(
                    WorksetStagerErrorCode::ArtifactFailure,
                    "Program baseline movie hash does not match");
                return completion;
            }
            savor::tas::DtmFile dtm;
            std::string reason;
            if (!dtm.load(artifact.movie_path->string()) ||
                !dtm.supports_gc_poll_editing(&reason) ||
                dtm.info().is_wii || dtm.info().controllers != 1 ||
                dtm.info().input_count != dtm.gc_poll_count())
            {
                completion.result = WorksetStagerResult::Failure(
                    WorksetStagerErrorCode::ArtifactFailure,
                    "Program baseline DTM is not a valid aligned GC-only movie: " +
                        reason);
                return completion;
            }
            const savor::tas::DtmInfo info = dtm.info();
            const std::string dtm_game_id(
                info.game_id.data(), info.game_id.size());
            if (dtm_game_id != artifact.compatibility.game_id)
            {
                completion.result = WorksetStagerResult::Failure(
                    WorksetStagerErrorCode::ArtifactFailure,
                    "Program baseline DTM belongs to another game");
                return completion;
            }
            if (artifact.kind == ProgramBaselineArtifactKind::ReadOnlyMovie)
            {
                const bool has_startup = !artifact.state_path.empty();
                if (info.starts_from_savestate != has_startup)
                {
                    completion.result = WorksetStagerResult::Failure(
                        WorksetStagerErrorCode::ArtifactFailure,
                        "Read-only-movie baseline startup-savestate declaration disagrees with its DTM header");
                    return completion;
                }
                if (has_startup && artifact.state_path !=
                    std::filesystem::path(artifact.movie_path->string() + ".sav"))
                {
                    completion.result = WorksetStagerResult::Failure(
                        WorksetStagerErrorCode::ArtifactFailure,
                        "Read-only-movie startup savestate must be the exact <dtm>.sav companion");
                    return completion;
                }
            }
        }
        evidence = std::move(observed);

        completion.result = WorksetStagerResult::Success();
        completion.package.emplace(HostStagedWorksetPackage{
            std::move(job.definition),
            baseline,
            std::move(evidence)});
        return completion;
    }
    catch (const std::exception& exception)
    {
        completion.result = WorksetStagerResult::Failure(
            WorksetStagerErrorCode::ArtifactFailure,
            exception.what());
        return completion;
    }
    catch (...)
    {
        completion.result = WorksetStagerResult::Failure(
            WorksetStagerErrorCode::ArtifactFailure,
            "Unknown host-only workset staging failure");
        return completion;
    }
}

} // namespace savor::runtime
