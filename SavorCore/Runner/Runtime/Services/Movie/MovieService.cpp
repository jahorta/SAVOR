#include "Runner/Runtime/Services/Movie/MovieService.h"

#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] MovieActivity ActivityOf(MovieCheckpointMode mode) noexcept
{
    switch (mode)
    {
    case MovieCheckpointMode::ReadOnlyPlayback:
        return MovieActivity::ReadOnlyPlayback;
    case MovieCheckpointMode::Recording:
        return MovieActivity::Recording;
    case MovieCheckpointMode::None:
        return MovieActivity::Inactive;
    }
    return MovieActivity::Inactive;
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
        .activity = activity_,
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
    if (MovieServiceResult observed = ValidateSnapshotFor(expected_movie);
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
    const MovieSnapshot observed = backend_.Snapshot();
    movie.current_frame = observed.current_frame;
    movie.current_input_count = observed.current_input_count;
    movie.cursor_known = true;

    preparation_ = MoviePreparationId(next_preparation_++);
    activity_ = MovieActivity::PreparedReadOnlyPlayback;
    active_movie_ = movie;
    prepared_starting_savestate_ = prepared.startup_savestate;
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
        activity_ != MovieActivity::PreparedReadOnlyPlayback ||
        !preparation_ || !active_movie_)
    {
        return std::nullopt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::PreparePlayback);
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
        activity_ != MovieActivity::PreparedReadOnlyPlayback ||
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
    if (MovieServiceResult observed = ValidateSnapshotFor(active_movie_);
        !observed.ok)
    {
        tainted_ = true;
        observed.integrity = GuestIntegrity::Unknown;
        receipt.result = std::move(observed);
        return receipt;
    }

    const MovieSnapshot observed = backend_.Snapshot();
    active_movie_->current_frame = observed.current_frame;
    active_movie_->current_input_count = observed.current_input_count;
    active_movie_->cursor_known = true;
    activity_ = MovieActivity::ReadOnlyPlayback;
    preparation_ = {};
    prepared_core_started_ = false;
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
    if (activity_ != MovieActivity::PreparedReadOnlyPlayback ||
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
    activity_ = MovieActivity::Inactive;
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
    receipt.activity = activity_;
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
    if (activity_ != MovieActivity::ReadOnlyPlayback)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No read-only movie playback is active");
        return receipt;
    }
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
    MovieServiceResult released = ReleaseReservation();
    if (!released.ok)
    {
        tainted_ = true;
        receipt.result = std::move(released);
        return receipt;
    }
    activity_ = MovieActivity::Inactive;
    active_movie_.reset();
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
    if (MovieServiceResult idle = ValidateIdle(); !idle.ok)
    {
        receipt.result = std::move(idle);
        return receipt;
    }
    if (MovieServiceResult acquired = AcquireReservation(); !acquired.ok)
    {
        receipt.result = std::move(acquired);
        return receipt;
    }
    MovieBackendResult started = backend_.BeginRecording();
    if (!started.ok)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            started.message.empty() ? "Movie recording start failed" :
                                      std::move(started.message),
            started.integrity);
        if (started.integrity == GuestIntegrity::Unknown)
            tainted_ = true;
        (void)ReleaseReservation();
        return receipt;
    }
    const MovieSnapshot observed = backend_.Snapshot();
    if (observed.activity != MovieActivity::Recording ||
        observed.read_only)
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend did not enter writable recording mode",
            GuestIntegrity::Unknown);
        return receipt;
    }
    activity_ = MovieActivity::Recording;
    active_movie_ = MovieCheckpointMetadata{
        .mode = MovieCheckpointMode::Recording,
    };
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
    receipt.reservation = reservation_;
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
    if (activity_ != MovieActivity::Recording)
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
    MovieCheckpointMetadata recorded;
    if (MovieServiceResult valid =
            ValidateDtm(request.dtm_path, recorded);
        !valid.ok)
    {
        tainted_ = true;
        receipt.result = std::move(valid);
        return receipt;
    }
    if (recorded.starts_from_savestate !=
        finalized.starting_savestate.has_value())
    {
        tainted_ = true;
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Final DTM and recording starting-state companion disagree",
            GuestIntegrity::Unknown);
        return receipt;
    }
    if (finalized.starting_savestate.has_value())
    {
        const std::filesystem::path expected =
            std::filesystem::path(request.dtm_path.string() + ".sav");
        if (*finalized.starting_savestate != expected ||
            !std::filesystem::is_regular_file(expected))
        {
            tainted_ = true;
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::ArtifactFailure,
                "Recording starting-state companion was not published at <dtm>.sav",
                GuestIntegrity::Unknown);
            return receipt;
        }
    }
    if (MovieServiceResult released = ReleaseReservation(); !released.ok)
    {
        tainted_ = true;
        receipt.result = std::move(released);
        return receipt;
    }
    activity_ = MovieActivity::Inactive;
    active_movie_.reset();
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
    if (activity_ != MovieActivity::Recording)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No movie recording is active");
        return receipt;
    }
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
    if (MovieServiceResult released = ReleaseReservation(); !released.ok)
    {
        tainted_ = true;
        receipt.result = std::move(released);
        return receipt;
    }
    activity_ = MovieActivity::Inactive;
    active_movie_.reset();
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
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
    if (activity_ == MovieActivity::Inactive || !active_movie_.has_value())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "No movie is active");
        return receipt;
    }

    MovieCheckpointMetadata checkpoint;
    if (activity_ == MovieActivity::ReadOnlyPlayback)
    {
        checkpoint = *active_movie_;
        const MovieSnapshot observed = backend_.Snapshot();
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

MovieSnapshot MovieService::snapshot() const
{
    if (!OnOwnerThread())
        return {};
    return backend_.Snapshot();
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
    if (context.movie.has_value() && !reservation_)
    {
        MovieServiceResult acquired = AcquireReservation();
        if (!acquired.ok)
            return acquired;
        acquired_for_restore_ = true;
    }
    original_activity_ = activity_;
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
        restore_prepared_ = false;
        return FromBackendResult(
            committed,
            "Movie savestate restore commit failed");
    }
    if (MovieServiceResult observed =
            ValidateSnapshotFor(context.movie);
        !observed.ok)
    {
        tainted_ = true;
        restore_prepared_ = false;
        observed.integrity = GuestIntegrity::Unknown;
        return observed;
    }

    if (context.movie.has_value())
    {
        activity_ = ActivityOf(context.movie->mode);
        MovieCheckpointMetadata reconciled = *context.movie;
        const MovieSnapshot observed = backend_.Snapshot();
        reconciled.current_frame = observed.current_frame;
        reconciled.current_input_count = observed.current_input_count;
        reconciled.cursor_known = true;
        active_movie_ = std::move(reconciled);
    }
    else
    {
        activity_ = MovieActivity::Inactive;
        active_movie_.reset();
        if (reservation_)
        {
            MovieServiceResult released = ReleaseReservation();
            if (!released.ok)
            {
                tainted_ = true;
                restore_prepared_ = false;
                released.integrity = GuestIntegrity::Unknown;
                return released;
            }
        }
    }
    restore_prepared_ = false;
    acquired_for_restore_ = false;
    original_movie_.reset();
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
    activity_ = original_activity_;
    active_movie_ = original_movie_;
    original_movie_.reset();
    restore_prepared_ = false;
    acquired_for_restore_ = false;
    if (!rolled_back.ok || !release.ok)
    {
        tainted_ = true;
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            !rolled_back.ok ?
                (rolled_back.message.empty() ?
                     "Movie savestate restore rollback failed" :
                     std::move(rolled_back.message)) :
                release.message,
            GuestIntegrity::Unknown);
    }
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
    if (activity_ != MovieActivity::Inactive || reservation_)
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

MovieServiceResult MovieService::ValidateSnapshotFor(
    const std::optional<MovieCheckpointMetadata>& movie) const
{
    const MovieSnapshot observed = backend_.Snapshot();
    if (!movie.has_value())
    {
        if (observed.activity != MovieActivity::Inactive)
        {
            return MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "Movie backend remained active after state replacement",
                GuestIntegrity::Unknown);
        }
        return MovieServiceResult::Success();
    }
    const MovieActivity expected = ActivityOf(movie->mode);
    if (observed.activity != expected ||
        (expected == MovieActivity::ReadOnlyPlayback &&
         !observed.read_only) ||
        (expected == MovieActivity::Recording &&
         observed.read_only))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend state does not match the checkpoint",
            GuestIntegrity::Unknown);
    }
    if (movie->cursor_known &&
        (observed.current_frame != movie->current_frame ||
         observed.current_input_count != movie->current_input_count))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend cursor does not match the checkpoint",
            GuestIntegrity::Unknown);
    }
    return MovieServiceResult::Success();
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
