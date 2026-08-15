#include "Runner/Runtime/Services/Movie/MovieService.h"

#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] MovieState StateOf(MovieCheckpointMode mode) noexcept
{
    switch (mode)
    {
    case MovieCheckpointMode::ReadOnlyPlayback:
        return MovieState::ReadOnlyPlayback;
    case MovieCheckpointMode::Recording:
        return MovieState::Recording;
    case MovieCheckpointMode::None:
        return MovieState::Inactive;
    }
    return MovieState::Inactive;
}

[[nodiscard]] bool HasDtmMagic(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    constexpr std::array<std::uint8_t, 4> magic{'D', 'T', 'M', 0x1a};
    return bytes.size() >= savor::tas::DtmFile::kMinHeader &&
        std::equal(magic.begin(), magic.end(), bytes.begin());
}

} // namespace

MovieService::MovieService(
    IMovieBackendPort& backend,
    IMovieInputReservationPort& reservations,
    std::function<WorksetEpoch()> active_workset_epoch,
    std::function<MovieServiceResult()> validate_before_core_stop,
    std::function<MovieServiceResult()> settle_after_core_stop,
    std::function<MovieServiceResult()> validate_after_core_start)
    : backend_(backend),
      reservations_(reservations),
      active_workset_epoch_(std::move(active_workset_epoch)),
      validate_before_core_stop_(std::move(validate_before_core_stop)),
      settle_after_core_stop_(std::move(settle_after_core_stop)),
      validate_after_core_start_(std::move(validate_after_core_start)),
      owner_thread_(std::this_thread::get_id())
{
}

WorksetEpoch MovieService::ActiveEpoch() const noexcept
{
    return active_workset_epoch_ ? active_workset_epoch_() : WorksetEpoch{};
}

MovieOperationReceipt MovieService::BaseReceipt(
    MovieOperation operation) const
{
    return {
        .result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            "Movie operation did not run"),
        .operation = operation,
        .state = state_,
        .workset_epoch = ActiveEpoch(),
        .reservation = reservation_,
    };
}

MovieOperationReceipt MovieService::PrepareReadOnlyPlayback(
    const MoviePlaybackRequest& request)
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::PreparePlayback;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::PreparePlayback);
    if (!ActiveEpoch())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Read-only playback requires an active workset");
        return receipt;
    }
    // Workset initialization already reconciled the paused baseline. Use the
    // owned idle state here; the newly prepared playback is verified after
    // Dolphin establishes it.
    const MovieStateSnapshot current = SnapshotState(ActiveEpoch());
    if (!current.result.ok)
    {
        receipt.result = current.result;
        return receipt;
    }
    if (MovieServiceResult idle = ValidateIdle(); !idle.ok)
    {
        receipt.result = std::move(idle);
        return receipt;
    }

    MovieCheckpointMetadata movie;
    if (MovieServiceResult valid =
            ValidateDtm(request.dtm_path, movie);
        !valid.ok)
    {
        receipt.result = std::move(valid);
        return receipt;
    }
    if (MovieServiceResult acquired = AcquireReservation(); !acquired.ok)
    {
        receipt.result = std::move(acquired);
        return receipt;
    }

    MoviePlaybackPrepareResult prepared =
        backend_.PrepareReadOnlyPlaybackForRestart(request.dtm_path);
    if (!prepared.result.ok)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            prepared.result.message.empty() ?
                "Movie backend failed to prepare playback" :
                std::move(prepared.result.message),
            prepared.result.integrity);
        if (prepared.result.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        MovieServiceResult released = ReleaseReservation();
        if (!released.ok)
        {
            tainted_ = true;
            receipt.result.code = MovieServiceErrorCode::IntegrityFailure;
            receipt.result.integrity = GuestIntegrity::Unknown;
            receipt.result.message += "; " + released.message;
        }
        return receipt;
    }
    const auto compensate_staging = [&](MovieServiceResult& primary) {
        const MovieBackendResult discarded =
            backend_.DiscardPreparedReadOnlyMovie();
        const MovieServiceResult released = ReleaseReservation();
        if (discarded.ok && released.ok)
            return;
        tainted_ = true;
        primary.code = MovieServiceErrorCode::IntegrityFailure;
        primary.integrity = GuestIntegrity::Unknown;
        if (!discarded.ok)
        {
            primary.message += "; " +
                (discarded.message.empty()
                    ? "prepared movie staging cleanup failed"
                    : discarded.message);
        }
        if (!released.ok)
        {
            primary.message += "; " +
                (released.message.empty()
                    ? "movie input reservation cleanup failed"
                    : released.message);
        }
    };
    if (movie.starts_from_savestate !=
        prepared.startup_savestate.has_value())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            movie.starts_from_savestate ?
                "DTM requires a startup savestate that is unavailable" :
                "Backend returned an unexpected startup savestate");
        compensate_staging(receipt.result);
        return receipt;
    }
    if (prepared.startup_savestate.has_value() &&
        !std::filesystem::is_regular_file(*prepared.startup_savestate))
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Movie startup savestate is not readable");
        compensate_staging(receipt.result);
        return receipt;
    }

    if (!validate_before_core_stop_)
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie preparation has no pre-stop validation authority",
            GuestIntegrity::Unknown);
        compensate_staging(receipt.result);
        return receipt;
    }
    if (next_preparation_ == 0)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie preparation identity space is exhausted");
        compensate_staging(receipt.result);
        return receipt;
    }
    if (MovieServiceResult ready = validate_before_core_stop_(); !ready.ok)
    {
        receipt.result = std::move(ready);
        if (receipt.result.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        compensate_staging(receipt.result);
        return receipt;
    }

    const MovieBackendResult stopped =
        backend_.StopCoreForPreparedReadOnlyMovie();
    if (!stopped.ok)
    {
        receipt.result = FromBackendResult(
            stopped,
            "Movie backend failed to stop the guest core");
        if (stopped.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        compensate_staging(receipt.result);
        return receipt;
    }
    if (!settle_after_core_stop_)
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Stopped movie core has no stop-point boundary authority",
            GuestIntegrity::Unknown);
        compensate_staging(receipt.result);
        return receipt;
    }
    MovieServiceResult settled = settle_after_core_stop_();
    if (!settled.ok)
    {
        tainted_ = true;
        settled.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(settled);
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        return receipt;
    }

    const MovieBackendResult started =
        backend_.StartPreparedReadOnlyMovieCorePaused();
    if (!started.ok)
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            started.message.empty()
                ? "Movie backend failed to start the prepared movie core paused"
                : started.message,
            GuestIntegrity::Unknown);
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        return receipt;
    }
    prepared_core_started_ = true;
    if (!validate_after_core_start_)
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Prepared movie core has no post-boot stop-point validation authority",
            GuestIntegrity::Unknown);
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        prepared_core_started_ = false;
        return receipt;
    }
    MovieServiceResult validated = validate_after_core_start_();
    if (!validated.ok)
    {
        tainted_ = true;
        validated.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(validated);
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        prepared_core_started_ = false;
        return receipt;
    }
    const std::optional<MovieCheckpointMetadata> expected_movie{movie};
    MovieBackendObservation backend_observation;
    if (MovieServiceResult observed = ValidateBackendFor(
            expected_movie,
            &backend_observation);
        !observed.ok)
    {
        tainted_ = true;
        observed.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(observed);
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        prepared_core_started_ = false;
        return receipt;
    }
    movie.current_frame = backend_observation.current_frame;
    movie.current_input_count = backend_observation.current_input_count;
    movie.cursor_known = true;

    preparation_ = MoviePreparationId(next_preparation_++);
    state_ = MovieState::PreparedReadOnlyPlayback;
    active_movie_ = movie;
    prepared_starting_savestate_ = prepared.startup_savestate;
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.workset_epoch = ActiveEpoch();
    receipt.reservation = reservation_;
    receipt.preparation = preparation_;
    receipt.dtm_sha256 = movie.dtm_sha256;
    receipt.artifact_path = request.dtm_path;
    receipt.starting_savestate = prepared.startup_savestate;
    return receipt;
}

std::optional<MovieOperationReceipt>
MovieService::PreparedReadOnlyPlaybackReceipt() const
{
    if (!OnOwnerThread() || !ActiveEpoch() ||
        state_ != MovieState::PreparedReadOnlyPlayback ||
        !preparation_ || !active_movie_)
    {
        return std::nullopt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::PreparePlayback);
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.workset_epoch = ActiveEpoch();
    receipt.reservation = reservation_;
    receipt.preparation = preparation_;
    receipt.dtm_sha256 = active_movie_->dtm_sha256;
    receipt.artifact_path = active_movie_->dtm_path;
    receipt.starting_savestate = prepared_starting_savestate_;
    return receipt;
}

MovieOperationReceipt MovieService::StartPreparedReadOnlyPlayback(
    MoviePreparationId preparation)
{
    MovieOperationReceipt receipt = BaseReceipt(MovieOperation::StartPlayback);
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    if (!ActiveEpoch() ||
        state_ != MovieState::PreparedReadOnlyPlayback ||
        !preparation || preparation != preparation_ || !active_movie_)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Movie playback requires the exact active prepared-movie handle");
        return receipt;
    }

    const MovieBackendResult activated =
        backend_.ActivatePreparedReadOnlyMoviePlayback();
    if (!activated.ok)
    {
        receipt.result = FromBackendResult(
            activated,
            "Movie backend failed to activate the prepared playback");
        if (receipt.result.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        return receipt;
    }
    MovieBackendObservation backend_observation;
    if (MovieServiceResult observed = ValidateBackendFor(
            active_movie_,
            &backend_observation);
        !observed.ok)
    {
        tainted_ = true;
        observed.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(observed);
        return receipt;
    }
    if (MovieServiceResult pause_at_end = AcquirePauseAtPlaybackEnd();
        !pause_at_end.ok)
    {
        tainted_ = true;
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        state_ = MovieState::Unknown;
        pause_at_end.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(pause_at_end);
        return receipt;
    }

    active_movie_->current_frame = backend_observation.current_frame;
    active_movie_->current_input_count = backend_observation.current_input_count;
    active_movie_->cursor_known = true;
    state_ = MovieState::ReadOnlyPlayback;
    preparation_ = {};
    prepared_core_started_ = false;
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.workset_epoch = ActiveEpoch();
    receipt.reservation = reservation_;
    receipt.dtm_sha256 = active_movie_->dtm_sha256;
    receipt.artifact_path = active_movie_->dtm_path;
    receipt.starting_savestate = prepared_starting_savestate_;
    prepared_starting_savestate_.reset();
    return receipt;
}

MovieOperationReceipt MovieService::AbandonPreparedReadOnlyPlayback(
    MoviePreparationId preparation) noexcept
{
    MovieOperationReceipt receipt = BaseReceipt(MovieOperation::PreparePlayback);
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    if (state_ != MovieState::PreparedReadOnlyPlayback ||
        !preparation || preparation != preparation_)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No matching prepared movie playback is active");
        return receipt;
    }

    const MovieBackendResult discarded = prepared_core_started_
        ? backend_.StopMovie()
        : backend_.DiscardPreparedReadOnlyMovie();
    const MovieServiceResult released = ReleaseReservation();
    state_ = MovieState::Inactive;
    preparation_ = {};
    active_movie_.reset();
    prepared_starting_savestate_.reset();
    prepared_core_started_ = false;
    if (discarded.ok && released.ok)
    {
        receipt.result = MovieServiceResult::Success();
    }
    else
    {
        tainted_ = true;
        std::string message = "Prepared movie cleanup failed";
        if (!discarded.ok && !discarded.message.empty())
            message += "; " + discarded.message;
        if (!released.ok && !released.message.empty())
            message += "; " + released.message;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            std::move(message),
            GuestIntegrity::Unknown);
    }
    receipt.state = state_;
    receipt.reservation = reservation_;
    return receipt;
}

MovieOperationReceipt MovieService::StopPlayback() noexcept
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::StopPlayback;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::StopPlayback);
    if (state_ != MovieState::ReadOnlyPlayback &&
        state_ != MovieState::PlaybackEnded)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No read-only movie playback is active");
        return receipt;
    }
    receipt.result = MovieServiceResult::Success();

    // Natural exhaustion has already detached Dolphin's native playback.  Its
    // retained service state only needs ownership cleanup; issuing another
    // native stop would incorrectly turn terminal observation into a command.
    if (state_ == MovieState::ReadOnlyPlayback)
    {
        MovieBackendResult stopped = backend_.StopMovie();
        if (!stopped.ok)
        {
            if (stopped.integrity == GuestIntegrity::Unknown)
                tainted_ = true;
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::BackendFailure,
                stopped.message.empty() ? "Movie playback stop failed" :
                                          std::move(stopped.message),
                stopped.integrity);
            return receipt;
        }
        MovieBackendObservation inactive;
        if (MovieServiceResult validated = ValidateBackendStateForCleanup(
                MovieState::Inactive,
                inactive);
            !validated.ok)
        {
            tainted_ = true;
            state_ = MovieState::Unknown;
            validated.integrity = GuestIntegrity::Unknown;
            receipt.result = std::move(validated);
        }
    }
    MovieServiceResult pause_at_end = ReleasePauseAtPlaybackEnd();
    if (!pause_at_end.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        pause_at_end.integrity = GuestIntegrity::Unknown;
        if (receipt.result.ok || receipt.result.message.empty())
            receipt.result = std::move(pause_at_end);
        else
            receipt.result.message += "; " + pause_at_end.message;
    }
    MovieServiceResult released = ReleaseReservation();
    if (!released.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        if (receipt.result.ok || receipt.result.message.empty())
            receipt.result = std::move(released);
        else
            receipt.result.message += "; " + released.message;
    }
    active_movie_.reset();
    if (!receipt.result.ok && state_ == MovieState::Unknown)
    {
        receipt.state = state_;
        receipt.reservation = reservation_;
        return receipt;
    }
    state_ = MovieState::Inactive;
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.reservation = {};
    return receipt;
}

MovieOperationReceipt MovieService::StartRecording(
    const MovieRecordingRequest&)
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::StartRecording;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::StartRecording);
    if (!ActiveEpoch())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Recording requires an active workset");
        return receipt;
    }
    // The paused execution boundary that admitted this action already
    // captured the authoritative cursor. Do not inspect Dolphin again before
    // requesting the transition; verify recording exactly once afterward.
    const MovieStateSnapshot current = SnapshotState(ActiveEpoch());
    if (!current.result.ok)
    {
        receipt.result = current.result;
        return receipt;
    }
    const bool branching = state_ == MovieState::ReadOnlyPlayback;
    if (!branching)
    {
        if (MovieServiceResult idle = ValidateIdle(); !idle.ok)
        {
            receipt.result = std::move(idle);
            return receipt;
        }
    }
    else if (!active_movie_ || !reservation_)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Read-only playback lacks its exact movie or reservation",
            GuestIntegrity::Unknown);
        tainted_ = true;
        return receipt;
    }

    recording_prefix_.reset();
    recording_prefix_input_count_ = 0;
    if (branching)
    {
        if (current.current_input_count >
            (std::numeric_limits<std::size_t>::max() -
             savor::tas::DtmFile::kMinHeader) / 8u)
        {
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::ArtifactFailure,
                "Read-only playback cursor exceeds the representable DTM prefix");
            return receipt;
        }
        const std::size_t prefix_size =
            savor::tas::DtmFile::kMinHeader +
            static_cast<std::size_t>(current.current_input_count) * 8u;
        if (prefix_size > active_movie_->dtm_bytes.size())
        {
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::ArtifactFailure,
                "Read-only playback cursor exceeds the admitted DTM payload");
            return receipt;
        }
        recording_prefix_ = *active_movie_;
        recording_prefix_input_count_ = current.current_input_count;
    }

    MovieBackendResult started = branching
        ? backend_.BranchReadOnlyPlaybackToRecording()
        : backend_.BeginRecording();
    if (!started.ok)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            started.message.empty() ? "Movie recording start failed" :
                                      std::move(started.message),
            started.integrity);
        if (started.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        recording_prefix_.reset();
        recording_prefix_input_count_ = 0;
        return receipt;
    }
    MovieBackendObservation observed;
    if (MovieServiceResult validated = ValidateBackendState(
            MovieState::Recording,
            observed);
        !validated.ok)
    {
        const MovieBackendResult cancelled = backend_.CancelRecording();
        MovieServiceResult released = MovieServiceResult::Success();
        if (cancelled.ok)
        {
            released = ReleaseReservation();
            RetireRecordingAfterBackendStop();
            receipt.state = state_;
            receipt.reservation = reservation_;
        }
        tainted_ = true;
        std::string message =
            validated.message.empty()
                ? "Movie backend did not enter writable recording mode"
                : std::move(validated.message);
        if (!cancelled.ok)
        {
            message += "; recording compensation failed: " +
                (cancelled.message.empty()
                    ? std::string("backend cancellation failed")
                    : cancelled.message);
        }
        else if (!released.ok)
        {
            message += "; input-reservation release failed: " +
                (released.message.empty()
                    ? std::string("reservation release failed")
                    : released.message);
        }
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            std::move(message),
            GuestIntegrity::Unknown);
        return receipt;
    }
    if (branching)
    {
        MovieServiceResult released = ReleaseReservation();
        if (!released.ok)
        {
            (void)backend_.CancelRecording();
            recording_prefix_.reset();
            recording_prefix_input_count_ = 0;
            tainted_ = true;
            released.integrity = GuestIntegrity::Unknown;
            receipt.result = std::move(released);
            return receipt;
        }
        MovieServiceResult pause_at_end = ReleasePauseAtPlaybackEnd();
        if (!pause_at_end.ok)
        {
            (void)backend_.CancelRecording();
            recording_prefix_.reset();
            recording_prefix_input_count_ = 0;
            tainted_ = true;
            state_ = MovieState::Unknown;
            pause_at_end.integrity = GuestIntegrity::Unknown;
            receipt.result = std::move(pause_at_end);
            return receipt;
        }
    }
    state_ = MovieState::Recording;
    active_movie_ = recording_prefix_.value_or(MovieCheckpointMetadata{});
    active_movie_->mode = MovieCheckpointMode::Recording;
    active_movie_->current_frame = observed.current_frame;
    active_movie_->current_input_count = observed.current_input_count;
    active_movie_->cursor_known = true;
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.reservation = {};
    return receipt;
}

MovieOperationReceipt MovieService::FinalizeRecording(
    const MovieFinalizeRequest& request)
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::FinalizeRecording;
        receipt.artifact_path = request.dtm_path;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::FinalizeRecording);
    receipt.artifact_path = request.dtm_path;
    if (state_ != MovieState::Recording)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No movie recording is active");
        return receipt;
    }
    if (request.dtm_path.empty() ||
        request.dtm_path.parent_path().empty() ||
        !std::filesystem::is_directory(request.dtm_path.parent_path()) ||
        std::filesystem::exists(request.dtm_path) ||
        std::filesystem::exists(
            std::filesystem::path(request.dtm_path.string() + ".sav")))
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidArgument,
            "Recording output must be a new caller-declared path");
        return receipt;
    }
    receipt.result = MovieServiceResult::Success();
    const MovieStateSnapshot current = ReconcilePausedState(ActiveEpoch());
    if (!current.result.ok)
    {
        receipt.result = current.result;
        return receipt;
    }

    if (recording_prefix_)
    {
        MovieCheckpointBackendResult pending =
            backend_.CaptureRecordingCheckpoint();
        if (!pending.result.ok)
        {
            receipt.result = FromBackendResult(
                pending.result,
                "Recording prefix checkpoint capture failed");
            if (pending.result.integrity == GuestIntegrity::Unknown)
                tainted_ = true;
            return receipt;
        }
        MovieCheckpointMetadata checkpoint = std::move(pending.checkpoint);
        checkpoint.mode = MovieCheckpointMode::Recording;
        checkpoint.cursor_known = true;
        if (MovieServiceResult valid = ValidateCheckpoint(checkpoint); !valid.ok)
        {
            receipt.result = std::move(valid);
            return receipt;
        }
        const std::size_t prefix_size =
            savor::tas::DtmFile::kMinHeader +
            static_cast<std::size_t>(recording_prefix_input_count_) * 8u;
        if (checkpoint.dtm_bytes.size() < prefix_size ||
            recording_prefix_->dtm_bytes.size() < prefix_size ||
            !std::equal(
                recording_prefix_->dtm_bytes.begin() +
                    savor::tas::DtmFile::kMinHeader,
                recording_prefix_->dtm_bytes.begin() + prefix_size,
                checkpoint.dtm_bytes.begin() +
                    savor::tas::DtmFile::kMinHeader))
        {
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "Recording no longer preserves the exact inherited DTM prefix",
                GuestIntegrity::Unknown);
            tainted_ = true;
            return receipt;
        }
    }

    MovieRecordingFinalizeResult finalized =
        backend_.FinalizeRecording(request.dtm_path);
    if (!finalized.result.ok)
    {
        if (finalized.result.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            finalized.result.message.empty() ?
                "Movie recording finalization failed" :
                std::move(finalized.result.message),
            finalized.result.integrity);
        return receipt;
    }

    const auto discard_finalized_artifacts = [&]() noexcept {
        std::string failure;
        for (const auto& path : {
                 request.dtm_path,
                 std::filesystem::path(request.dtm_path.string() + ".sav")})
        {
            std::error_code error;
            (void)std::filesystem::remove(path, error);
            if (error)
            {
                if (!failure.empty()) failure += "; ";
                failure += path.string() + ": " + error.message();
            }
        }
        return failure;
    };
    const auto fail_after_backend_finalize =
        [&](MovieServiceResult failure) -> MovieOperationReceipt {
            const std::string cleanup = discard_finalized_artifacts();
            if (!cleanup.empty())
                failure.message += "; finalized-artifact cleanup failed: " +
                    cleanup;
            failure.integrity = GuestIntegrity::Unknown;
            tainted_ = true;
            receipt.result = std::move(failure);
            return receipt;
        };

    MovieBackendObservation inactive;
    if (MovieServiceResult validated = ValidateBackendState(
            MovieState::Inactive,
            inactive);
        !validated.ok)
    {
        state_ = MovieState::Unknown;
        return fail_after_backend_finalize(std::move(validated));
    }

    // A successful backend finalization has already ended Dolphin's movie
    // session. Retire the service-side recording state before validating the
    // resulting files so a later artifact failure cannot make resource unwind
    // attempt to cancel a recording that no longer exists.
    MovieServiceResult released = ReleaseReservation();
    RetireRecordingAfterBackendStop();
    receipt.state = state_;
    receipt.reservation = reservation_;
    if (!released.ok)
    {
        return fail_after_backend_finalize(std::move(released));
    }

    MovieCheckpointMetadata recorded;
    if (MovieServiceResult valid =
            ValidateDtm(request.dtm_path, recorded);
        !valid.ok)
    {
        return fail_after_backend_finalize(std::move(valid));
    }
    if (recorded.starts_from_savestate !=
        finalized.starting_savestate.has_value())
    {
        return fail_after_backend_finalize(MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Final DTM and recording starting-state companion disagree",
            GuestIntegrity::Unknown));
    }
    if (finalized.starting_savestate.has_value())
    {
        const std::filesystem::path expected =
            std::filesystem::path(request.dtm_path.string() + ".sav");
        if (*finalized.starting_savestate != expected ||
            !std::filesystem::is_regular_file(expected))
        {
            return fail_after_backend_finalize(MovieServiceResult::Failure(
                MovieServiceErrorCode::ArtifactFailure,
                "Recording starting-state companion was not published at <dtm>.sav",
                GuestIntegrity::Unknown));
        }
    }
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.reservation = {};
    receipt.dtm_sha256 = recorded.dtm_sha256;
    receipt.starting_savestate = finalized.starting_savestate;
    return receipt;
}

MovieOperationReceipt MovieService::CancelRecording() noexcept
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::CancelRecording;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::CancelRecording);
    if (state_ != MovieState::Recording)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No movie recording is active");
        return receipt;
    }
    receipt.result = MovieServiceResult::Success();
    MovieBackendResult cancelled = backend_.CancelRecording();
    if (!cancelled.ok)
    {
        if (cancelled.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            cancelled.message.empty() ? "Movie recording cancellation failed" :
                                        std::move(cancelled.message),
            cancelled.integrity);
        return receipt;
    }
    MovieBackendObservation inactive;
    if (MovieServiceResult validated = ValidateBackendStateForCleanup(
            MovieState::Inactive,
            inactive);
        !validated.ok)
    {
        state_ = MovieState::Unknown;
        tainted_ = true;
        validated.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(validated);
    }
    if (MovieServiceResult released = ReleaseReservation(); !released.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        if (receipt.result.ok || receipt.result.message.empty())
            receipt.result = std::move(released);
        else
            receipt.result.message += "; " + released.message;
    }
    active_movie_.reset();
    recording_prefix_.reset();
    recording_prefix_input_count_ = 0;
    if (!receipt.result.ok && state_ == MovieState::Unknown)
    {
        receipt.state = state_;
        receipt.reservation = reservation_;
        return receipt;
    }
    state_ = MovieState::Inactive;
    receipt.result = MovieServiceResult::Success();
    receipt.state = state_;
    receipt.reservation = {};
    return receipt;
}

MovieCheckpointReceipt MovieService::CaptureCheckpoint()
{
    MovieCheckpointReceipt receipt;
    if (!OnOwnerThread())
    {
        receipt.result = WrongThread();
        return receipt;
    }
    receipt.workset_epoch = ActiveEpoch();
    const MovieStateSnapshot observed =
        ReconcilePausedState(receipt.workset_epoch);
    if (!observed.result.ok)
    {
        receipt.result = observed.result;
        return receipt;
    }
    if ((state_ != MovieState::ReadOnlyPlayback &&
         state_ != MovieState::PlaybackEnded &&
         state_ != MovieState::Recording) ||
        !active_movie_.has_value())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No movie is active");
        return receipt;
    }

    MovieCheckpointMetadata checkpoint;
    if (state_ == MovieState::ReadOnlyPlayback ||
        state_ == MovieState::PlaybackEnded)
    {
        checkpoint = *active_movie_;
        checkpoint.current_frame = observed.current_frame;
        checkpoint.current_input_count = observed.current_input_count;
        checkpoint.cursor_known = true;
    }
    else
    {
        MovieCheckpointBackendResult captured =
            backend_.CaptureRecordingCheckpoint();
        if (!captured.result.ok)
        {
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::BackendFailure,
                captured.result.message.empty() ?
                    "Recording checkpoint capture failed" :
                    std::move(captured.result.message),
                captured.result.integrity);
            if (captured.result.integrity == GuestIntegrity::Unknown)
                tainted_ = true;
            return receipt;
        }
        checkpoint = std::move(captured.checkpoint);
        checkpoint.mode = MovieCheckpointMode::Recording;
        checkpoint.cursor_known = true;
    }
    if (MovieServiceResult valid = ValidateCheckpoint(checkpoint); !valid.ok)
    {
        receipt.result = std::move(valid);
        return receipt;
    }
    receipt.result = MovieServiceResult::Success();
    receipt.checkpoint = std::move(checkpoint);
    return receipt;
}

MovieStateSnapshot MovieService::SnapshotState(
    WorksetEpoch expected_epoch) const
{
    if (!OnOwnerThread())
    {
        return {
            .result = WrongThread(),
            .workset_epoch = expected_epoch,
            .state = MovieState::Unknown,
        };
    }
    const WorksetEpoch active_epoch = ActiveEpoch();
    if (!expected_epoch || expected_epoch != active_epoch)
    {
        return {
            .result = MovieServiceResult::Failure(
                MovieServiceErrorCode::InvalidState,
                "Movie snapshot does not belong to the active workset"),
            .workset_epoch = expected_epoch,
            .state = MovieState::Unknown,
        };
    }
    if (tainted_)
    {
        return {
            .result = MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "MovieService is tainted",
                GuestIntegrity::Unknown),
            .workset_epoch = expected_epoch,
            .state = MovieState::Unknown,
        };
    }
    const bool playback_owned =
        state_ == MovieState::ReadOnlyPlayback ||
        state_ == MovieState::PlaybackEnded;
    if (playback_owned != pause_at_playback_end_owned_)
    {
        return {
            .result = MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "MovieService pause-at-playback-end ownership is inconsistent",
                GuestIntegrity::Unknown),
            .workset_epoch = expected_epoch,
            .state = MovieState::Unknown,
        };
    }

    MovieStateSnapshot snapshot{
        .result = MovieServiceResult::Success(),
        .workset_epoch = active_epoch,
        .state = state_,
        .read_only = state_ != MovieState::Recording,
    };
    if (active_movie_ && active_movie_->cursor_known)
    {
        snapshot.current_frame = active_movie_->current_frame;
        snapshot.current_input_count = active_movie_->current_input_count;
    }
    return snapshot;
}

MovieStateSnapshot MovieService::ReconcilePausedState(
    WorksetEpoch expected_epoch)
{
    if (!OnOwnerThread())
        return ObservationFailure(WrongThread(), expected_epoch);
    const WorksetEpoch active_epoch = ActiveEpoch();
    if (!expected_epoch || expected_epoch != active_epoch)
    {
        return ObservationFailure(
            MovieServiceResult::Failure(
                MovieServiceErrorCode::InvalidState,
                "Movie observation does not belong to the active workset"),
            expected_epoch);
    }
    if (tainted_)
    {
        return ObservationFailure(
            MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "MovieService is tainted",
                GuestIntegrity::Unknown),
            expected_epoch);
    }

    const MovieBackendObservation observed =
        backend_.ObserveMovieWhilePaused();
    if (!observed.result.ok)
    {
        MovieServiceResult failure = FromBackendResult(
            observed.result,
            "Movie backend observation failed");
        if (failure.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        return ObservationFailure(std::move(failure), expected_epoch);
    }
    if (observed.playing && observed.recording)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        return ObservationFailure(
            MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "Movie backend reported simultaneous playback and recording",
                GuestIntegrity::Unknown),
            expected_epoch);
    }

    MovieStateSnapshot snapshot{
        .result = MovieServiceResult::Success(),
        .workset_epoch = active_epoch,
        .state = state_,
        .read_only = observed.read_only,
        .current_frame = observed.current_frame,
        .current_input_count = observed.current_input_count,
    };
    const auto fail_contradiction = [&](std::string message) {
        tainted_ = true;
        state_ = MovieState::Unknown;
        snapshot.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            std::move(message),
            GuestIntegrity::Unknown);
        snapshot.state = state_;
        return snapshot;
    };

    switch (state_)
    {
    case MovieState::Inactive:
        if (observed.playing || observed.recording)
            return fail_contradiction(
                "Movie backend became active without MovieService ownership");
        break;
    case MovieState::PreparedReadOnlyPlayback:
        if (!observed.playing || observed.recording || !observed.read_only)
            return fail_contradiction(
                "Prepared read-only playback no longer matches the movie backend");
        break;
    case MovieState::ReadOnlyPlayback:
        if (!pause_at_playback_end_owned_)
            return fail_contradiction(
                "Read-only playback lacks pause-at-playback-end ownership");
        if (observed.playing && !observed.recording && observed.read_only)
        {
            if (active_movie_)
            {
                active_movie_->current_frame = observed.current_frame;
                active_movie_->current_input_count =
                    observed.current_input_count;
                active_movie_->cursor_known = true;
            }
            break;
        }
        if (!observed.playing && !observed.recording && observed.read_only)
        {
            state_ = MovieState::PlaybackEnded;
            if (active_movie_)
            {
                active_movie_->current_frame = observed.current_frame;
                active_movie_->current_input_count =
                    observed.current_input_count;
                active_movie_->cursor_known = true;
            }
            snapshot.state = state_;
            break;
        }
        return fail_contradiction(
            "Read-only playback changed to an unrequested movie mode");
    case MovieState::Recording:
        if (pause_at_playback_end_owned_)
            return fail_contradiction(
                "Movie recording retained pause-at-playback-end ownership");
        if (reservation_)
            return fail_contradiction(
                "Movie recording retained an exclusive controller reservation");
        if (!observed.recording || observed.playing || observed.read_only)
            return fail_contradiction(
                "Movie recording disappeared or changed mode unexpectedly");
        if (active_movie_)
        {
            active_movie_->current_frame = observed.current_frame;
            active_movie_->current_input_count = observed.current_input_count;
            active_movie_->cursor_known = true;
        }
        break;
    case MovieState::PlaybackEnded:
        if (!pause_at_playback_end_owned_)
            return fail_contradiction(
                "Ended playback lacks pause-at-playback-end ownership");
        if (observed.playing || observed.recording || !observed.read_only)
            return fail_contradiction(
                "Ended playback lost its retained read-only backend evidence");
        if (active_movie_ && active_movie_->cursor_known)
        {
            snapshot.current_frame = active_movie_->current_frame;
            snapshot.current_input_count = active_movie_->current_input_count;
        }
        break;
    case MovieState::Unknown:
        return fail_contradiction(
            "MovieService has an unknown owned state");
    }
    return snapshot;
}

MovieServiceResult MovieService::PrepareSavestateRestore(
    const SavestateMovieRestoreContext& context)
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!context.workset_epoch || context.workset_epoch != ActiveEpoch())
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Savestate movie restore does not belong to the active workset");
    }
    if (restore_prepared_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "A savestate movie restore is already prepared");
    }
    if (context.movie.has_value() &&
        context.movie->mode == MovieCheckpointMode::Recording &&
        context.external_artifact)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::Unsupported,
            "Cold restoration of an in-progress recording is unsupported");
    }
    const bool restoring_read_only = context.movie.has_value() &&
        context.movie->mode == MovieCheckpointMode::ReadOnlyPlayback;
    const bool restoring_recording = context.movie.has_value() &&
        context.movie->mode == MovieCheckpointMode::Recording;
    if (restoring_recording &&
        context.movie->recording_workset_epoch != ActiveEpoch())
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Recording checkpoint does not belong to the active workset");
    }
    // Read-only playback owns Dolphin's controller source exclusively.
    // Recording observes InputArbiter publications and must never acquire that
    // reservation.
    if (restoring_read_only && !reservation_)
    {
        MovieServiceResult acquired = AcquireReservation();
        if (!acquired.ok)
            return acquired;
        acquired_for_restore_ = true;
    }
    original_state_ = state_;
    original_movie_ = active_movie_;
    MovieBackendResult prepared =
        backend_.PrepareSavestateRestore(context);
    if (!prepared.ok)
    {
        if (acquired_for_restore_)
        {
            (void)ReleaseReservation();
            acquired_for_restore_ = false;
        }
        original_movie_.reset();
        original_state_ = MovieState::Inactive;
        return FromBackendResult(
            prepared,
            "Movie backend failed to prepare savestate history");
    }
    restore_prepared_ = true;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::CommitSavestateRestore(
    const SavestateMovieRestoreContext& context)
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!restore_prepared_ || context.workset_epoch != ActiveEpoch())
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Savestate movie restore was not prepared for this workset");
    }
    MovieBackendResult committed =
        backend_.CommitSavestateRestore(context);
    if (!committed.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        active_movie_.reset();
        recording_prefix_.reset();
        recording_prefix_input_count_ = 0;
        MovieServiceResult failure = FromBackendResult(
            committed,
            "Movie savestate restore commit failed");
        if (reservation_)
        {
            MovieServiceResult released = ReleaseReservation();
            if (!released.ok)
                failure.message += "; " + released.message;
        }
        restore_prepared_ = false;
        acquired_for_restore_ = false;
        original_movie_.reset();
        original_state_ = MovieState::Inactive;
        failure.integrity = GuestIntegrity::Unknown;
        return failure;
    }
    MovieBackendObservation backend_observation;
    if (MovieServiceResult observed =
            ValidateBackendFor(context.movie, &backend_observation);
        !observed.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        active_movie_.reset();
        recording_prefix_.reset();
        recording_prefix_input_count_ = 0;
        if (reservation_)
        {
            MovieServiceResult released = ReleaseReservation();
            if (!released.ok)
                observed.message += "; " + released.message;
        }
        restore_prepared_ = false;
        acquired_for_restore_ = false;
        original_movie_.reset();
        original_state_ = MovieState::Inactive;
        observed.integrity = GuestIntegrity::Unknown;
        return observed;
    }

    if (context.movie.has_value())
    {
        MovieCheckpointMetadata reconciled = *context.movie;
        reconciled.current_frame = backend_observation.current_frame;
        reconciled.current_input_count =
            backend_observation.current_input_count;
        reconciled.cursor_known = true;
        const MovieState restored_state = StateOf(context.movie->mode);
        if (restored_state == MovieState::ReadOnlyPlayback &&
            !pause_at_playback_end_owned_)
        {
            MovieServiceResult pause_at_end = AcquirePauseAtPlaybackEnd();
            if (!pause_at_end.ok)
            {
                tainted_ = true;
                state_ = MovieState::Unknown;
                active_movie_.reset();
                recording_prefix_.reset();
                recording_prefix_input_count_ = 0;
                restore_prepared_ = false;
                acquired_for_restore_ = false;
                original_movie_.reset();
                original_state_ = MovieState::Inactive;
                pause_at_end.integrity = GuestIntegrity::Unknown;
                return pause_at_end;
            }
        }
        else if (restored_state != MovieState::ReadOnlyPlayback &&
                 pause_at_playback_end_owned_)
        {
            MovieServiceResult pause_at_end = ReleasePauseAtPlaybackEnd();
            if (!pause_at_end.ok)
            {
                tainted_ = true;
                state_ = MovieState::Unknown;
                active_movie_.reset();
                recording_prefix_.reset();
                recording_prefix_input_count_ = 0;
                restore_prepared_ = false;
                acquired_for_restore_ = false;
                original_movie_.reset();
                original_state_ = MovieState::Inactive;
                pause_at_end.integrity = GuestIntegrity::Unknown;
                return pause_at_end;
            }
        }
        if (restored_state == MovieState::Recording && reservation_)
        {
            MovieServiceResult released = ReleaseReservation();
            if (!released.ok)
            {
                tainted_ = true;
                state_ = MovieState::Unknown;
                active_movie_.reset();
                recording_prefix_.reset();
                recording_prefix_input_count_ = 0;
                restore_prepared_ = false;
                acquired_for_restore_ = false;
                original_movie_.reset();
                original_state_ = MovieState::Inactive;
                released.integrity = GuestIntegrity::Unknown;
                return released;
            }
        }
        state_ = restored_state;
        active_movie_ = reconciled;
        if (restored_state == MovieState::Recording)
        {
            // The restored recording history up to its exact cursor is the
            // immutable prefix that later finalization must preserve.
            recording_prefix_ = reconciled;
            recording_prefix_input_count_ = reconciled.current_input_count;
        }
        else
        {
            recording_prefix_.reset();
            recording_prefix_input_count_ = 0;
        }
    }
    else
    {
        if (pause_at_playback_end_owned_)
        {
            MovieServiceResult pause_at_end = ReleasePauseAtPlaybackEnd();
            if (!pause_at_end.ok)
            {
                tainted_ = true;
                state_ = MovieState::Unknown;
                restore_prepared_ = false;
                acquired_for_restore_ = false;
                original_movie_.reset();
                original_state_ = MovieState::Inactive;
                pause_at_end.integrity = GuestIntegrity::Unknown;
                return pause_at_end;
            }
        }
        state_ = MovieState::Inactive;
        active_movie_.reset();
        recording_prefix_.reset();
        recording_prefix_input_count_ = 0;
        if (reservation_)
        {
            MovieServiceResult released = ReleaseReservation();
            if (!released.ok)
            {
                tainted_ = true;
                state_ = MovieState::Unknown;
                restore_prepared_ = false;
                acquired_for_restore_ = false;
                original_movie_.reset();
                original_state_ = MovieState::Inactive;
                released.integrity = GuestIntegrity::Unknown;
                return released;
            }
        }
    }
    restore_prepared_ = false;
    acquired_for_restore_ = false;
    original_movie_.reset();
    original_state_ = MovieState::Inactive;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::RollbackSavestateRestore(
    const SavestateMovieRestoreContext& context) noexcept
{
    if (!OnOwnerThread())
        return WrongThread();
    if (!restore_prepared_)
        return MovieServiceResult::Success();
    MovieBackendResult rolled_back =
        backend_.RollbackSavestateRestore(context);
    MovieServiceResult release = MovieServiceResult::Success();
    if (acquired_for_restore_)
        release = ReleaseReservation();
    restore_prepared_ = false;
    acquired_for_restore_ = false;
    if (!rolled_back.ok || !release.ok)
    {
        tainted_ = true;
        state_ = MovieState::Unknown;
        active_movie_.reset();
        recording_prefix_.reset();
        recording_prefix_input_count_ = 0;
        original_movie_.reset();
        original_state_ = MovieState::Inactive;
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            !rolled_back.ok ?
                (rolled_back.message.empty() ?
                     "Movie savestate restore rollback failed" :
                     std::move(rolled_back.message)) :
                release.message,
            GuestIntegrity::Unknown);
    }
    state_ = original_state_;
    active_movie_ = original_movie_;
    original_movie_.reset();
    original_state_ = MovieState::Inactive;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::ValidateIdle() const
{
    if (tainted_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "MovieService is tainted",
            GuestIntegrity::Unknown);
    }
    if (state_ != MovieState::Inactive || reservation_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Another movie lifecycle is active");
    }
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::AcquireReservation()
{
    if (reservation_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Movie input reservation is already held");
    }
    MovieInputReservationReceipt acquired =
        reservations_.AcquireUnsuspendableMovieReservation();
    if (!acquired.result.ok || !acquired.reservation)
    {
        return acquired.result.ok ?
            MovieServiceResult::Failure(
                MovieServiceErrorCode::ReservationFailure,
                "Input collaborator returned an invalid movie reservation") :
            std::move(acquired.result);
    }
    reservation_ = acquired.reservation;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::ReleaseReservation() noexcept
{
    if (!reservation_)
        return MovieServiceResult::Success();
    const MovieReservationId releasing = reservation_;
    MovieServiceResult result =
        reservations_.ReleaseMovieReservation(releasing);
    if (result.ok)
        reservation_ = {};
    return result;
}

MovieServiceResult MovieService::AcquirePauseAtPlaybackEnd()
{
    if (pause_at_playback_end_owned_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Pause-at-playback-end ownership is already active");
    }
    const MovieBackendResult acquired =
        backend_.AcquirePauseAtPlaybackEnd();
    if (!acquired.ok)
    {
        return FromBackendResult(
            acquired,
            "Movie backend could not enable pause at playback end");
    }
    pause_at_playback_end_owned_ = true;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::ReleasePauseAtPlaybackEnd() noexcept
{
    if (!pause_at_playback_end_owned_)
        return MovieServiceResult::Success();
    const MovieBackendResult released =
        backend_.ReleasePauseAtPlaybackEnd();
    if (!released.ok)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            released.message.empty()
                ? "Movie backend could not restore pause-at-playback-end configuration"
                : released.message,
            GuestIntegrity::Unknown);
    }
    pause_at_playback_end_owned_ = false;
    return MovieServiceResult::Success();
}

void MovieService::RetireRecordingAfterBackendStop() noexcept
{
    state_ = MovieState::Inactive;
    active_movie_.reset();
    recording_prefix_.reset();
    recording_prefix_input_count_ = 0;
}

MovieServiceResult MovieService::ValidateBackendState(
    MovieState expected,
    MovieBackendObservation& observation) const
{
    observation = backend_.ObserveMovieWhilePaused();
    if (!observation.result.ok)
    {
        return FromBackendResult(
            observation.result,
            "Movie backend observation failed");
    }
    if (observation.playing && observation.recording)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend reported simultaneous playback and recording",
            GuestIntegrity::Unknown);
    }
    const bool matches =
        (expected == MovieState::Inactive &&
         !observation.playing && !observation.recording) ||
        (expected == MovieState::ReadOnlyPlayback &&
         observation.playing && !observation.recording &&
         observation.read_only) ||
        (expected == MovieState::Recording &&
         !observation.playing && observation.recording &&
         !observation.read_only);
    if (!matches)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend physical state does not match the requested transition",
            GuestIntegrity::Unknown);
    }
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::ValidateBackendStateForCleanup(
    MovieState expected,
    MovieBackendObservation& observation) const noexcept
{
    try
    {
        return ValidateBackendState(expected, observation);
    }
    catch (const std::exception& error)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            std::string("Movie backend observation threw during cleanup: ") +
                error.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend observation threw during cleanup",
            GuestIntegrity::Unknown);
    }
}

MovieServiceResult MovieService::ValidateBackendFor(
    const std::optional<MovieCheckpointMetadata>& movie,
    MovieBackendObservation* observation_out) const
{
    MovieBackendObservation observed;
    const MovieState expected = movie.has_value()
        ? StateOf(movie->mode)
        : MovieState::Inactive;
    if (MovieServiceResult valid = ValidateBackendState(expected, observed);
        !valid.ok)
    {
        return valid;
    }
    if (movie.has_value() && movie->cursor_known &&
        (observed.current_frame != movie->current_frame ||
         observed.current_input_count != movie->current_input_count))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend cursor does not match the checkpoint",
            GuestIntegrity::Unknown);
    }
    if (observation_out)
        *observation_out = observed;
    return MovieServiceResult::Success();
}

MovieStateSnapshot MovieService::ObservationFailure(
    MovieServiceResult result,
    WorksetEpoch expected_epoch)
{
    return {
        .result = std::move(result),
        .workset_epoch = expected_epoch,
        .state = MovieState::Unknown,
    };
}

MovieServiceResult MovieService::ValidateDtm(
    const std::filesystem::path& path,
    MovieCheckpointMetadata& metadata) const
{
    savor::tas::DtmFile dtm;
    if (path.empty() || !dtm.load(path.string()))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Unable to load DTM " + path.string());
    }
    const savor::tas::DtmValidationReport validation = dtm.validate();
    if (validation.has_error())
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "DTM validation failed for " + path.string());
    }
    const savor::tas::DtmInfo info = dtm.info();
    metadata.mode = MovieCheckpointMode::ReadOnlyPlayback;
    metadata.dtm_sha256 = dtm.compute_sha256();
    metadata.game_id.assign(
        info.game_id.data(),
        info.game_id.size());
    metadata.dtm_bytes = dtm.bytes();
    metadata.dtm_path = path;
    metadata.starts_from_savestate = info.starts_from_savestate;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::ValidateCheckpoint(
    MovieCheckpointMetadata& metadata) const
{
    if (!metadata.HasMovie() ||
        !HasDtmMagic(metadata.dtm_bytes))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Movie checkpoint does not contain a valid DTM");
    }
    const std::string actual = hash::sha256(
        metadata.dtm_bytes.data(),
        metadata.dtm_bytes.size());
    if (!metadata.dtm_sha256.empty() &&
        metadata.dtm_sha256 != actual)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Movie checkpoint DTM SHA-256 does not match");
    }
    metadata.dtm_sha256 = actual;
    const std::string dtm_game_id(
        reinterpret_cast<const char*>(metadata.dtm_bytes.data() + 4),
        6);
    if (!metadata.game_id.empty() &&
        metadata.game_id != dtm_game_id)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Movie checkpoint game ID does not match its DTM");
    }
    metadata.game_id = dtm_game_id;
    metadata.starts_from_savestate = metadata.dtm_bytes[12] != 0;
    return MovieServiceResult::Success();
}

MovieServiceResult MovieService::FromBackendResult(
    const MovieBackendResult& result,
    std::string fallback)
{
    return MovieServiceResult::Failure(
        result.integrity == GuestIntegrity::Unknown
            ? MovieServiceErrorCode::IntegrityFailure
            : MovieServiceErrorCode::BackendFailure,
        result.message.empty() ? std::move(fallback) : result.message,
        result.integrity);
}

bool MovieService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

MovieServiceResult MovieService::WrongThread()
{
    return MovieServiceResult::Failure(
        MovieServiceErrorCode::InvalidState,
        "MovieService operation used the wrong actor thread");
}

} // namespace savor::runtime
