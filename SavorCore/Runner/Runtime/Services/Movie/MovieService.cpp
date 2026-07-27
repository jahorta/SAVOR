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

[[nodiscard]] MovieOperationReceipt BaseReceipt(
    MovieOperation operation,
    StateService& state_service,
    MovieActivity activity,
    MovieReservationId reservation)
{
    return {
        .result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            "Movie operation did not run"),
        .operation = operation,
        .activity = activity,
        .state_epoch = state_service.current_epoch(),
        .reservation = reservation,
    };
}

} // namespace

MovieService::MovieService(
    IMovieBackendPort& backend,
    IMovieInputReservationPort& reservations,
    StateService& state_service)
    : backend_(backend),
      reservations_(reservations),
      state_service_(state_service),
      owner_thread_(std::this_thread::get_id())
{
}

MovieServiceResult MovieService::RegisterForStateReplacement()
{
    if (!OnOwnerThread())
        return WrongThread();
    if (registered_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "MovieService is already registered with StateService");
    }
    StateServiceResult result =
        state_service_.RegisterParticipant(*this);
    if (!result.ok)
        return FromStateResult(result);
    registered_ = true;
    return MovieServiceResult::Success();
}

MovieOperationReceipt MovieService::StartReadOnlyPlayback(
    const MoviePlaybackRequest& request)
{
    if (!OnOwnerThread())
    {
        MovieOperationReceipt receipt;
        receipt.operation = MovieOperation::StartPlayback;
        receipt.result = WrongThread();
        return receipt;
    }
    MovieOperationReceipt receipt = BaseReceipt(
        MovieOperation::StartPlayback,
        state_service_,
        activity_,
        reservation_);
    if (!registered_)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "MovieService must be registered before playback");
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
        backend_.PrepareReadOnlyPlaybackBeforeBoot(request.dtm_path);
    if (!prepared.result.ok)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::BackendFailure,
            prepared.result.message.empty() ?
                "Movie backend failed to prepare playback" :
                std::move(prepared.result.message),
            prepared.result.integrity);
        if (prepared.result.integrity == StateIntegrity::Unknown)
            tainted_ = true;
        (void)ReleaseReservation();
        return receipt;
    }
    if (movie.starts_from_savestate !=
        prepared.startup_savestate.has_value())
    {
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            movie.starts_from_savestate ?
                "DTM requires a startup savestate that is unavailable" :
                "Backend returned an unexpected startup savestate");
        return receipt;
    }
    if (prepared.startup_savestate.has_value() &&
        !std::filesystem::is_regular_file(*prepared.startup_savestate))
    {
        (void)backend_.StopMovie();
        (void)ReleaseReservation();
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::ArtifactFailure,
            "Movie startup savestate is not readable");
        return receipt;
    }

    StateBootRequest boot = request.boot;
    boot.startup_savestate = prepared.startup_savestate;
    boot.movie = movie;
    starting_playback_ = true;
    pending_start_movie_ = movie;
    const StateOperationReceipt state = state_service_.is_open() ?
        state_service_.Reboot(boot) :
        state_service_.Boot(boot);
    starting_playback_ = false;
    pending_start_movie_.reset();
    if (!state.result.ok)
    {
        const MovieBackendResult stopped = backend_.StopMovie();
        const MovieServiceResult released = ReleaseReservation();
        receipt.result = FromStateResult(state.result);
        if (!stopped.ok || !released.ok)
        {
            tainted_ = true;
            receipt.result = MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                "Playback boot failed and movie cleanup was not proven",
                StateIntegrity::Unknown);
        }
        receipt.state_epoch = state.resulting_epoch;
        return receipt;
    }

    if (MovieServiceResult observed =
            ValidateSnapshotFor(movie);
        !observed.ok)
    {
        tainted_ = true;
        receipt.result = std::move(observed);
        receipt.state_epoch = state.resulting_epoch;
        return receipt;
    }
    activity_ = MovieActivity::ReadOnlyPlayback;
    if (!active_movie_.has_value())
    {
        const MovieSnapshot observed = backend_.Snapshot();
        movie.current_frame = observed.current_frame;
        movie.current_input_count = observed.current_input_count;
        movie.cursor_known = true;
        active_movie_ = movie;
    }
    receipt.result = MovieServiceResult::Success();
    receipt.activity = activity_;
    receipt.state_epoch = state.resulting_epoch;
    receipt.reservation = reservation_;
    receipt.dtm_sha256 = movie.dtm_sha256;
    receipt.artifact_path = request.dtm_path;
    receipt.starting_savestate = prepared.startup_savestate;
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
        MovieOperation::StopPlayback,
        state_service_,
        activity_,
        reservation_);
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
        if (stopped.integrity == StateIntegrity::Unknown)
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
        MovieOperation::StartRecording,
        state_service_,
        activity_,
        reservation_);
    if (!registered_)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "MovieService must be registered before recording");
        return receipt;
    }
    if (!state_service_.is_open() || state_service_.is_tainted())
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Recording requires a clean open StateService");
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
        if (started.integrity == StateIntegrity::Unknown)
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
            StateIntegrity::Unknown);
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
        MovieOperation::FinalizeRecording,
        state_service_,
        activity_,
        reservation_);
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
        if (finalized.result.integrity == StateIntegrity::Unknown)
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
            StateIntegrity::Unknown);
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
                StateIntegrity::Unknown);
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
        MovieOperation::CancelRecording,
        state_service_,
        activity_,
        reservation_);
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
        if (cancelled.integrity == StateIntegrity::Unknown)
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
    receipt.state_epoch = state_service_.current_epoch();
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
            if (captured.result.integrity == StateIntegrity::Unknown)
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

StateServiceResult MovieService::PrepareStateReplacement(
    const StateReplacementContext& context)
{
    if (!OnOwnerThread())
        return WrongThreadParticipant();
    if (replacement_prepared_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            "Movie state replacement is already prepared");
    }
    if (context.movie.has_value() &&
        context.movie->mode == MovieCheckpointMode::Recording &&
        context.external_artifact)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::Unsupported,
            "Cold restoration of an in-progress recording is unsupported");
    }
    if (context.movie.has_value() && !reservation_)
    {
        MovieServiceResult acquired = AcquireReservation();
        if (!acquired.ok)
        {
            return StateServiceResult::Failure(
                StateServiceErrorCode::ParticipantFailure,
                acquired.message,
                acquired.integrity);
        }
        acquired_for_replacement_ = true;
    }
    original_activity_ = activity_;
    original_movie_ = active_movie_;
    MovieBackendResult prepared =
        backend_.PrepareStateReplacement(context);
    if (!prepared.ok)
    {
        if (acquired_for_replacement_)
        {
            (void)ReleaseReservation();
            acquired_for_replacement_ = false;
        }
        return FromBackendResult(prepared);
    }
    replacement_prepared_ = true;
    return StateServiceResult::Success();
}

StateServiceResult MovieService::CommitStateReplacement(
    const StateReplacementContext& context)
{
    if (!OnOwnerThread())
        return WrongThreadParticipant();
    if (!replacement_prepared_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            "Movie state replacement was not prepared");
    }
    MovieBackendResult committed =
        backend_.CommitStateReplacement(context);
    if (!committed.ok)
    {
        tainted_ = true;
        replacement_prepared_ = false;
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            committed.message.empty() ?
                "Movie state replacement commit failed" :
                std::move(committed.message),
            StateIntegrity::Unknown);
    }
    if (MovieServiceResult observed =
            ValidateSnapshotFor(context.movie);
        !observed.ok)
    {
        tainted_ = true;
        replacement_prepared_ = false;
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            observed.message,
            StateIntegrity::Unknown);
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
                replacement_prepared_ = false;
                return StateServiceResult::Failure(
                    StateServiceErrorCode::IntegrityFailure,
                    released.message,
                    StateIntegrity::Unknown);
            }
        }
    }
    replacement_prepared_ = false;
    acquired_for_replacement_ = false;
    original_movie_.reset();
    return StateServiceResult::Success();
}

StateServiceResult MovieService::RollbackStateReplacement(
    const StateReplacementContext& context) noexcept
{
    if (!OnOwnerThread())
        return WrongThreadParticipant();
    if (!replacement_prepared_)
        return StateServiceResult::Success();
    MovieBackendResult rolled_back =
        backend_.RollbackStateReplacement(context);
    MovieServiceResult release = MovieServiceResult::Success();
    if (acquired_for_replacement_)
        release = ReleaseReservation();
    activity_ = original_activity_;
    active_movie_ = original_movie_;
    original_movie_.reset();
    replacement_prepared_ = false;
    acquired_for_replacement_ = false;
    if (!rolled_back.ok || !release.ok)
    {
        tainted_ = true;
        return StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            !rolled_back.ok ?
                (rolled_back.message.empty() ?
                     "Movie replacement rollback failed" :
                     std::move(rolled_back.message)) :
                release.message,
            StateIntegrity::Unknown);
    }
    return StateServiceResult::Success();
}

MovieServiceResult MovieService::ValidateIdle() const
{
    if (tainted_)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "MovieService is tainted",
            StateIntegrity::Unknown);
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
                StateIntegrity::Unknown);
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
            StateIntegrity::Unknown);
    }
    if (movie->cursor_known &&
        (observed.current_frame != movie->current_frame ||
         observed.current_input_count != movie->current_input_count))
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            "Movie backend cursor does not match the checkpoint",
            StateIntegrity::Unknown);
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

MovieServiceResult MovieService::FromStateResult(
    const StateServiceResult& result)
{
    MovieServiceErrorCode code = MovieServiceErrorCode::StateFailure;
    if (result.code == StateServiceErrorCode::Unsupported)
        code = MovieServiceErrorCode::Unsupported;
    else if (result.code == StateServiceErrorCode::InvalidArgument)
        code = MovieServiceErrorCode::InvalidArgument;
    else if (result.code == StateServiceErrorCode::InvalidState)
        code = MovieServiceErrorCode::InvalidState;
    else if (result.code == StateServiceErrorCode::ArtifactFailure)
        code = MovieServiceErrorCode::ArtifactFailure;
    else if (result.code == StateServiceErrorCode::IntegrityFailure)
        code = MovieServiceErrorCode::IntegrityFailure;
    return MovieServiceResult::Failure(
        code,
        result.message,
        result.integrity);
}

StateServiceResult MovieService::FromBackendResult(
    const MovieBackendResult& result)
{
    return StateServiceResult::Failure(
        result.integrity == StateIntegrity::Unknown ?
            StateServiceErrorCode::IntegrityFailure :
            StateServiceErrorCode::ParticipantFailure,
        result.message.empty() ? "Movie backend operation failed" :
                                 result.message,
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

StateServiceResult MovieService::WrongThreadParticipant() const
{
    return StateServiceResult::Failure(
        StateServiceErrorCode::ParticipantFailure,
        "MovieService state participant used the wrong actor thread");
}

} // namespace savor::runtime
