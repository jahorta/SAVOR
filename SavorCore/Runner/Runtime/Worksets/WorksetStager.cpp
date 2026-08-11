#include "WorksetStager.h"

#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Tas/DtmFile.h"
#include "Utils/Hash.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../../../../SavorProbe/ProbeRuntime.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
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

[[nodiscard]] std::optional<std::string> ReadBoundedTextFile(
    const std::filesystem::path& path,
    std::size_t maximum_bytes,
    std::string& error)
{
    std::error_code filesystem_error;
    const std::uintmax_t size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size > maximum_bytes)
    {
        error = filesystem_error
            ? "Capture profile sidecar could not be inspected"
            : "Capture profile sidecar exceeds the runtime limit";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "Capture profile sidecar could not be opened";
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input || input.gcount() !=
            static_cast<std::streamsize>(text.size()))
    {
        error = "Capture profile sidecar could not be read completely";
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] bool AddRequestedBreakpointProgress(
    savor::probe::Profile& profile,
    const progress::ProgressPlanV1& plan,
    std::string& error)
{
    for (const progress::ProgressPointBindingV1& point : plan.points)
    {
        if (point.provider !=
                progress::ProgressProviderKind::BreakpointCapture ||
            !point.breakpoint_pc)
        {
            continue;
        }
        const bool already_present = std::ranges::any_of(
            profile.probes,
            [&](const savor::probe::ProbeDefinition& probe) {
                return probe.kind == savor::probe::ProbeKind::Pc &&
                    probe.address == *point.breakpoint_pc &&
                    savor::probe::has_subscription(
                        probe.subscriptions,
                        savor::probe::Subscription::Progress) &&
                    probe.progress_formatter ==
                        point.formatter.canonical_id;
            });
        if (already_present)
            continue;
        auto probe = progress::BuildBreakpointProgressProbeV1(point);
        if (!probe)
        {
            error = "Requested breakpoint progress provider is unavailable";
            return false;
        }
        profile.probes.push_back(std::move(*probe));
    }
    return true;
}

struct CurrentModuleIdentity
{
    std::string sha256;
    std::string error;
};

[[nodiscard]] const CurrentModuleIdentity& CurrentCaptureModule()
{
    static const CurrentModuleIdentity identity = [] {
        CurrentModuleIdentity value;
        value.sha256 = savor::probe::current_module_sha256(
            &value.error);
        return value;
    }();
    return identity;
}

[[nodiscard]] bool IsRegisteredSemanticPc(std::uint32_t pc)
{
    static const auto catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    return std::ranges::any_of(
        catalog.manifests,
        [pc](const program::CapabilityPackManifest& manifest) {
            return std::ranges::any_of(
                manifest.semantic_points,
                [pc](const program::SemanticPointDescriptor& point) {
                    return point.kind ==
                            program::SemanticPointKind::ProgramCounter &&
                        point.pc == pc;
                });
        });
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
        for (const auto& point : job.definition.progress_plan.points)
        {
            for (const std::uint32_t pc :
                 point.runtime_sample_trigger_pcs)
            {
                if (!IsRegisteredSemanticPc(pc))
                {
                    completion.result = Invalid(
                        "Progress plan references an unregistered semantic trigger PC");
                    return completion;
                }
            }
        }
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

        std::optional<HostStagedCaptureProfile> staged_capture;
        std::optional<savor::probe::Profile> observation_profile;
        std::optional<std::filesystem::path> capture_output_directory;
        std::string bound_profile_json;
        if (job.definition.capture)
        {
            const WorksetCaptureBindingV1& binding =
                *job.definition.capture;
            if (binding.storage == CaptureProfileStorageV1::Inline)
            {
                bound_profile_json = binding.profile_json;
            }
            else
            {
                std::string read_error;
                const auto loaded = ReadBoundedTextFile(
                    binding.profile_sidecar_path,
                    limits_.maximum_capture_profile_bytes,
                    read_error);
                if (!loaded)
                {
                    completion.result = WorksetStagerResult::Failure(
                        WorksetStagerErrorCode::ArtifactFailure,
                        std::move(read_error));
                    return completion;
                }
                bound_profile_json = *loaded;
            }
            if (bound_profile_json.size() >
                    limits_.maximum_capture_profile_bytes ||
                hash::sha256(
                    bound_profile_json.data(), bound_profile_json.size()) !=
                    binding.profile_sha256)
            {
                completion.result = Invalid(
                    "Capture profile content does not match its immutable binding");
                return completion;
            }
            savor::probe::ProfileParseResult parsed =
                savor::probe::parse_profile_json(bound_profile_json);
            if (!parsed.profile)
            {
                completion.result = Invalid(
                    savor::probe::format_profile_errors(parsed));
                return completion;
            }
            if (parsed.profile->expected_module_sha256 !=
                    binding.expected_module_sha256)
            {
                completion.result = Invalid(
                    "Capture profile module identity disagrees with its workset binding");
                return completion;
            }
            const CurrentModuleIdentity& module =
                CurrentCaptureModule();
            if (module.sha256.size() != 64 ||
                binding.expected_module_sha256 != module.sha256)
            {
                completion.result = Invalid(
                    module.error.empty()
                        ? "Capture profile targets a different worker module"
                        : module.error);
                return completion;
            }
            const progress::ProgressValidationResult formatter_validation =
                progress::ValidateCaptureProgressFormatters(
                    *parsed.profile,
                    &job.definition.progress_plan);
            if (!formatter_validation)
            {
                completion.result = Invalid(
                    formatter_validation.message.empty()
                        ? "Capture profile progress formatter is invalid"
                        : formatter_validation.message);
                return completion;
            }
            if (progress::ComputeResolvedObservationHashV1(
                    *parsed.profile,
                    job.definition.progress_plan) !=
                binding.resolved_observation_sha256)
            {
                completion.result = Invalid(
                    "Capture profile resolved observation hash does not match");
                return completion;
            }
            observation_profile = std::move(*parsed.profile);
            capture_output_directory = binding.output_directory;
        }
        const bool needs_breakpoint_progress = std::ranges::any_of(
            job.definition.progress_plan.points,
            [](const progress::ProgressPointBindingV1& point) {
                return point.provider ==
                    progress::ProgressProviderKind::BreakpointCapture;
            });
        if (needs_breakpoint_progress && !observation_profile)
        {
            const CurrentModuleIdentity& module =
                CurrentCaptureModule();
            if (module.sha256.size() != 64)
            {
                completion.result = WorksetStagerResult::Failure(
                    WorksetStagerErrorCode::ComponentFailure,
                    module.error.empty()
                        ? "Worker module hash is unavailable for canonical progress"
                        : module.error);
                return completion;
            }
            observation_profile.emplace();
            observation_profile->name = "canonical-workset-progress";
            observation_profile->expected_module_sha256 = module.sha256;
        }
        if (observation_profile)
        {
            std::string progress_probe_error;
            if (!AddRequestedBreakpointProgress(
                    *observation_profile,
                    job.definition.progress_plan,
                    progress_probe_error))
            {
                completion.result = Invalid(progress_probe_error);
                return completion;
            }
            std::string effective_json =
                savor::probe::serialize_profile_json(
                    *observation_profile);
            savor::probe::ProfileParseResult effective =
                savor::probe::parse_profile_json(effective_json);
            if (!effective.profile)
            {
                completion.result = Invalid(
                    savor::probe::format_profile_errors(effective));
                return completion;
            }
            const progress::ProgressValidationResult effective_validation =
                progress::ValidateCaptureProgressFormatters(
                    *effective.profile,
                    &job.definition.progress_plan);
            if (!effective_validation)
            {
                completion.result = Invalid(
                    effective_validation.message);
                return completion;
            }
            staged_capture.emplace(HostStagedCaptureProfile{
                .profile_json = std::move(effective_json),
                .profile = std::move(*effective.profile),
                .output_directory =
                    std::move(capture_output_directory),
            });
        }

        if (job.definition.phase_invocation.program_package.identity.canonical_id ==
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
        if (job.definition.phase_invocation.program_package.identity.canonical_id ==
            "savor.full_phase.tas_movie_checkpoint_sterilize")
        {
            const ProgramBaselineArtifact& baseline =
                job.definition.baseline.artifact;
            if (baseline.kind != ProgramBaselineArtifactKind::Savestate ||
                !baseline.movie_path ||
                job.definition.baseline.components.size() != 1 ||
                job.definition.baseline.components.front() !=
                    MakeTasMovieCheckpointSterilizationBaselineComponent())
            {
                completion.result = Invalid(
                    "TAS Movie checkpoint sterilization requires one exact paired-savestate baseline and detach component");
                return completion;
            }
            for (const WorksetItemTemplate& item : job.definition.items)
            {
                tasmovie::TasMovieCheckpointSterilizationRequestV1 request;
                std::string diagnostic;
                if (!tasmovie::DecodeTasMovieCheckpointSterilizationExecutionInputV1(
                        item.execution.input_payload, request, &diagnostic) ||
                    std::filesystem::path(request.source_savestate_path) !=
                        baseline.state_path ||
                    std::filesystem::path(request.source_dtm_path) !=
                        *baseline.movie_path)
                {
                    completion.result = Invalid(
                        diagnostic.empty()
                            ? "TAS Movie checkpoint sterilization scalar input disagrees with its paired baseline"
                            : std::move(diagnostic));
                    return completion;
                }
            }
        }
        else if (std::ranges::any_of(
                     job.definition.baseline.components,
                     [](const ProgramBaselineComponent& component) {
                         return component.canonical_id ==
                             kTasMovieCheckpointSterilizationBaselineComponentId;
                     }))
        {
            completion.result = Invalid(
                "The checkpoint sterilization baseline component is restricted to its exact Full Phase");
            return completion;
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
            std::move(evidence),
            std::move(staged_capture)});
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
