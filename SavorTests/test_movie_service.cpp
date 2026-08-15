#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Movie/MovieService.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        path_ = std::filesystem::temp_directory_path() /
            ("savor-movie-service-" + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> Dtm(
    bool from_state = false,
    std::size_t input_count = 0)
{
    std::vector<std::uint8_t> bytes(256 + input_count * 8, 0);
    bytes[0] = 'D'; bytes[1] = 'T'; bytes[2] = 'M'; bytes[3] = 0x1a;
    bytes[4] = 'G'; bytes[5] = 'E'; bytes[6] = 'A';
    bytes[7] = 'E'; bytes[8] = '8'; bytes[9] = 'E';
    bytes[11] = 1;
    bytes[12] = from_state ? 1 : 0;
    for (std::size_t input = 0; input < input_count; ++input)
    {
        for (std::size_t byte = 0; byte < 8; ++byte)
            bytes[256 + input * 8 + byte] =
                static_cast<std::uint8_t>(input * 16 + byte + 1);
    }
    return bytes;
}

void Write(const std::filesystem::path& path,
           const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}

MovieCheckpointMetadata RecordingCheckpoint(
    std::vector<std::uint8_t> bytes,
    std::uint64_t frame,
    std::uint64_t input_count)
{
    MovieCheckpointMetadata result;
    result.mode = MovieCheckpointMode::Recording;
    result.dtm_sha256 = hash::sha256(bytes.data(), bytes.size());
    result.game_id = "GEAE8E";
    result.dtm_bytes = std::move(bytes);
    result.starts_from_savestate = false;
    result.cursor_known = true;
    result.current_frame = frame;
    result.current_input_count = input_count;
    return result;
}

class Backend final : public IMovieBackendPort
{
public:
    MoviePlaybackPrepareResult PrepareReadOnlyPlaybackForRestart(
        const std::filesystem::path& path) override
    {
        calls.push_back("prepare");
        prepared = path;
        return {prepare_result, startup};
    }
    MovieBackendResult StopCoreForPreparedReadOnlyMovie() override
    {
        calls.push_back("stop-core");
        return stop_core_result;
    }
    MovieBackendResult StartPreparedReadOnlyMovieCorePaused() override
    {
        calls.push_back("start-core");
        if (start_core_result.ok)
        {
            observation.playing = true;
            observation.recording = false;
            observation.read_only = true;
        }
        return start_core_result;
    }
    MovieBackendResult ActivatePreparedReadOnlyMoviePlayback() override
    {
        calls.push_back("activate-playback");
        return activation_result;
    }
    MovieBackendResult DiscardPreparedReadOnlyMovie() noexcept override
    {
        calls.push_back("discard-prepared");
        prepared.clear();
        return MovieBackendResult::Success();
    }
    MovieBackendResult StopMovie() noexcept override
    {
        calls.push_back("stop");
        SetInactive();
        return MovieBackendResult::Success();
    }
    MovieBackendResult BeginRecording() override
    {
        observation.playing = false;
        observation.recording = true;
        observation.read_only = false;
        return MovieBackendResult::Success();
    }

    MovieBackendResult BranchReadOnlyPlaybackToRecording() override
    {
        calls.push_back("branch-recording");
        if (!branch_result.ok)
            return branch_result;
        if (!observation.playing || observation.recording ||
            !observation.read_only)
            return MovieBackendResult::Failure("playback unavailable");
        if (branch_observation_override)
            observation = *branch_observation_override;
        else
        {
            observation.playing = false;
            observation.recording = true;
            observation.read_only = false;
        }
        return MovieBackendResult::Success();
    }
    MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& path) override
    {
        calls.push_back("finalize-recording");
        if (!finalize_result.ok)
            return {finalize_result, std::nullopt};
        Write(path, finalized_dtm);
        SetInactive();
        return {finalize_result, finalized_starting_savestate};
    }
    MovieBackendResult CancelRecording() noexcept override
    {
        calls.push_back("cancel-recording");
        SetInactive();
        return MovieBackendResult::Success();
    }
    MovieBackendObservation ObserveMovieWhilePaused() const override
    {
        ++observation_count;
        if (throw_on_observe)
            throw std::runtime_error("injected observation failure");
        return observation;
    }
    MovieBackendResult AcquirePauseAtPlaybackEnd() override
    {
        calls.push_back("acquire-pause-at-end");
        if (!pause_at_end_result.ok)
            return pause_at_end_result;
        if (pause_at_end_owned)
            return MovieBackendResult::Failure("pause at end already owned");
        pause_at_end_owned = true;
        return MovieBackendResult::Success();
    }
    MovieBackendResult ReleasePauseAtPlaybackEnd() noexcept override
    {
        calls.push_back("release-pause-at-end");
        pause_at_end_owned = false;
        return MovieBackendResult::Success();
    }
    MovieCheckpointBackendResult CaptureRecordingCheckpoint() override
    {
        calls.push_back("capture-recording-checkpoint");
        return {capture_result, capture_checkpoint};
    }
    MovieBackendResult PrepareSavestateRestore(
        const SavestateMovieRestoreContext& context) override
    {
        calls.push_back("prepare-savestate");
        last_restore = context;
        return restore_prepare_result;
    }
    MovieBackendResult CommitSavestateRestore(
        const SavestateMovieRestoreContext& context) override
    {
        calls.push_back("commit-savestate");
        if (context.movie)
        {
            observation.playing = context.movie->mode ==
                MovieCheckpointMode::ReadOnlyPlayback;
            observation.recording = context.movie->mode ==
                MovieCheckpointMode::Recording;
            observation.read_only = observation.playing;
            observation.current_frame = context.movie->current_frame;
            observation.current_input_count =
                context.movie->current_input_count;
        }
        else
        {
            SetInactive();
        }
        return restore_commit_result;
    }
    MovieBackendResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext&) noexcept override
    {
        calls.push_back("rollback-savestate");
        return restore_rollback_result;
    }

    void SetInactive()
    {
        observation = MovieBackendObservation{
            .result = MovieBackendResult::Success(),
            .read_only = true,
        };
    }

    std::vector<std::string> calls;
    std::filesystem::path prepared;
    std::optional<std::filesystem::path> startup;
    std::optional<SavestateMovieRestoreContext> last_restore;
    MovieBackendObservation observation{
        .result = MovieBackendResult::Success(),
        .read_only = true,
    };
    bool throw_on_observe = false;
    mutable std::uint64_t observation_count = 0;
    bool pause_at_end_owned = false;
    MovieBackendResult pause_at_end_result = MovieBackendResult::Success();
    MovieBackendResult prepare_result = MovieBackendResult::Success();
    MovieBackendResult stop_core_result = MovieBackendResult::Success();
    MovieBackendResult start_core_result = MovieBackendResult::Success();
    MovieBackendResult activation_result = MovieBackendResult::Success();
    MovieBackendResult branch_result = MovieBackendResult::Success();
    std::optional<MovieBackendObservation> branch_observation_override;
    MovieBackendResult finalize_result = MovieBackendResult::Failure("unused");
    MovieBackendResult capture_result = MovieBackendResult::Failure("unused");
    MovieCheckpointMetadata capture_checkpoint;
    std::vector<std::uint8_t> finalized_dtm;
    std::optional<std::filesystem::path> finalized_starting_savestate;
    MovieBackendResult restore_prepare_result = MovieBackendResult::Success();
    MovieBackendResult restore_commit_result = MovieBackendResult::Success();
    MovieBackendResult restore_rollback_result = MovieBackendResult::Success();
};

class Reservations final : public IMovieInputReservationPort
{
public:
    MovieInputReservationReceipt AcquireUnsuspendableMovieReservation() override
    {
        held = MovieReservationId(1);
        return {MovieServiceResult::Success(), held};
    }
    MovieServiceResult ReleaseMovieReservation(
        MovieReservationId reservation) noexcept override
    {
        if (reservation != held)
            return MovieServiceResult::Failure(
                MovieServiceErrorCode::ReservationFailure,
                "wrong reservation");
        held = {};
        return MovieServiceResult::Success();
    }
    MovieReservationId held;
};

TEST(MovieService, InitializationStartsPausedCoreBeforePlaybackActivation)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(17);
    int before_stop = 0;
    int after_stop = 0;
    int after_start = 0;
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend,
        reservations,
        [&] { return epoch; },
        [&] {
            ++before_stop;
            return MovieServiceResult::Success();
        },
        [&] {
            ++after_stop;
            return MovieServiceResult::Success();
        },
        [&] {
            ++after_start;
            return MovieServiceResult::Success();
        });

    const MovieOperationReceipt prepared =
        service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    const MovieOperationReceipt result =
        service.StartPreparedReadOnlyPlayback(prepared.preparation);

    ASSERT_TRUE(result.result.ok) << result.result.message;
    EXPECT_EQ(result.workset_epoch, epoch);
    EXPECT_EQ(before_stop, 1);
    EXPECT_EQ(after_stop, 1);
    EXPECT_EQ(after_start, 1);
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{
                  "prepare", "stop-core", "start-core",
                  "activate-playback", "acquire-pause-at-end"}));
    EXPECT_TRUE(result.reservation);
}

TEST(MovieService, UnknownCoreStartFailureTaintsMovieServiceWithoutEpochChange)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(9);
    Backend backend;
    backend.start_core_result = MovieBackendResult::Failure(
        "restart failed", GuestIntegrity::Unknown);
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const MovieOperationReceipt prepared =
        service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    EXPECT_FALSE(prepared.result.ok);
    EXPECT_EQ(prepared.workset_epoch, epoch);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_FALSE(reservations.held);
    EXPECT_FALSE(prepared.preparation);
}

TEST(MovieService, PlaybackActivationFailureLeavesPreparedCoreRecoverable)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(10);
    Backend backend;
    backend.activation_result = MovieBackendResult::Failure(
        "not paused", GuestIntegrity::Preserved);
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const MovieOperationReceipt prepared =
        service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    const MovieOperationReceipt activated =
        service.StartPreparedReadOnlyPlayback(prepared.preparation);

    EXPECT_FALSE(activated.result.ok);
    EXPECT_EQ(activated.result.integrity, GuestIntegrity::Preserved);
    EXPECT_FALSE(service.is_tainted());
    EXPECT_TRUE(reservations.held);
    EXPECT_TRUE(service.AbandonPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    EXPECT_FALSE(reservations.held);
}

TEST(MovieService, NaturalPlaybackExhaustionRetainsCursorUntilOwnedCleanup)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm(false, 4));
    WorksetEpoch epoch(11);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.playing = false;
    backend.observation.current_frame = 44;
    backend.observation.current_input_count = 4;

    const MovieStateSnapshot ended = service.ReconcilePausedState(epoch);

    ASSERT_TRUE(ended.result.ok) << ended.result.message;
    EXPECT_EQ(ended.state, MovieState::PlaybackEnded);
    EXPECT_EQ(ended.current_frame, 44u);
    EXPECT_EQ(ended.current_input_count, 4u);
    EXPECT_EQ(service.state(), MovieState::PlaybackEnded);
    EXPECT_TRUE(service.reservation());
    const MovieCheckpointReceipt checkpoint = service.CaptureCheckpoint();
    ASSERT_TRUE(checkpoint.result.ok) << checkpoint.result.message;
    ASSERT_TRUE(checkpoint.checkpoint);
    EXPECT_EQ(checkpoint.checkpoint->current_frame, 44u);
    EXPECT_EQ(checkpoint.checkpoint->current_input_count, 4u);

    const MovieOperationReceipt stopped = service.StopPlayback();
    ASSERT_TRUE(stopped.result.ok) << stopped.result.message;
    EXPECT_EQ(stopped.state, MovieState::Inactive);
    EXPECT_FALSE(service.reservation());
    EXPECT_EQ(std::ranges::count(backend.calls, std::string("stop")), 0);
}

TEST(MovieService, CachedStateSnapshotPerformsNoBackendInspection)
{
    WorksetEpoch epoch(18);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend,
        reservations,
        [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });
    const std::uint64_t observations_before = backend.observation_count;

    const MovieStateSnapshot snapshot = service.SnapshotState(epoch);

    ASSERT_TRUE(snapshot.result.ok) << snapshot.result.message;
    EXPECT_EQ(snapshot.state, MovieState::Inactive);
    EXPECT_EQ(backend.observation_count, observations_before);
}

TEST(MovieService, PlaybackCannotEndWithoutRetainedReadOnlyEvidence)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm(false, 1));
    WorksetEpoch epoch(14);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.playing = false;
    backend.observation.recording = false;
    backend.observation.read_only = false;

    const MovieStateSnapshot result = service.ReconcilePausedState(epoch);

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.state, MovieState::Unknown);
    EXPECT_EQ(service.state(), MovieState::Unknown);
    EXPECT_TRUE(service.is_tainted());
}

TEST(MovieService, PlaybackCleanupContainsBackendObservationException)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(15);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.throw_on_observe = true;

    const MovieOperationReceipt result = service.StopPlayback();

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Unknown);
    EXPECT_EQ(service.state(), MovieState::Unknown);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(std::ranges::count(backend.calls, std::string("stop")), 1);
}

TEST(MovieService, RecordingCleanupContainsBackendObservationException)
{
    WorksetEpoch epoch(16);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    ASSERT_TRUE(service.StartRecording().result.ok);
    ASSERT_EQ(service.state(), MovieState::Recording);
    backend.throw_on_observe = true;

    const MovieOperationReceipt result = service.CancelRecording();

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Unknown);
    EXPECT_EQ(service.state(), MovieState::Unknown);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("cancel-recording")), 1);
}

TEST(MovieService, ContradictoryNativeModesTaintOwnedState)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(13);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.recording = true;

    const MovieStateSnapshot result = service.ReconcilePausedState(epoch);

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Unknown);
    EXPECT_EQ(result.state, MovieState::Unknown);
    EXPECT_EQ(service.state(), MovieState::Unknown);
    EXPECT_TRUE(service.is_tainted());
}

TEST(MovieService, AbandoningPreparedPausedCoreReleasesResourcesCleanly)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm());
    WorksetEpoch epoch(12);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const MovieOperationReceipt prepared =
        service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;

    const MovieOperationReceipt abandoned =
        service.AbandonPreparedReadOnlyPlayback(prepared.preparation);

    EXPECT_TRUE(abandoned.result.ok) << abandoned.result.message;
    EXPECT_EQ(abandoned.workset_epoch, epoch);
    EXPECT_FALSE(service.is_tainted());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{
                  "prepare", "stop-core", "start-core", "stop"}));
}

TEST(MovieService,
     BranchesExactPlaybackCursorToRecordingAndFinalizesVerifiedPrefix)
{
    TemporaryDirectory temp;
    const auto source = temp.path() / "source.dtm";
    const auto output = temp.path() / "recorded.dtm";
    Write(source, Dtm(false, 3));
    WorksetEpoch epoch(22);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = source});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    const auto playback = service.StartPreparedReadOnlyPlayback(
        prepared.preparation);
    ASSERT_TRUE(playback.result.ok) << playback.result.message;
    ASSERT_TRUE(reservations.held);
    backend.observation.current_frame = 17;
    backend.observation.current_input_count = 2;
    const MovieStateSnapshot paused =
        service.ReconcilePausedState(epoch);
    ASSERT_TRUE(paused.result.ok) << paused.result.message;
    const std::uint64_t observations_before_recording =
        backend.observation_count;

    const auto recording = service.StartRecording();
    ASSERT_TRUE(recording.result.ok) << recording.result.message;
    EXPECT_EQ(recording.state, MovieState::Recording);
    EXPECT_EQ(service.state(), MovieState::Recording);
    EXPECT_FALSE(recording.reservation);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(backend.observation.current_frame, 17u);
    EXPECT_EQ(backend.observation.current_input_count, 2u);
    EXPECT_EQ(
        backend.observation_count,
        observations_before_recording + 1u);

    backend.capture_result = MovieBackendResult::Success();
    backend.capture_checkpoint = RecordingCheckpoint(Dtm(false, 3), 23, 3);
    backend.finalize_result = MovieBackendResult::Success();
    backend.finalized_dtm = Dtm(false, 3);
    const auto finalized = service.FinalizeRecording({.dtm_path = output});

    ASSERT_TRUE(finalized.result.ok) << finalized.result.message;
    EXPECT_EQ(finalized.state, MovieState::Inactive);
    EXPECT_EQ(service.state(), MovieState::Inactive);
    EXPECT_TRUE(std::filesystem::is_regular_file(output));
    EXPECT_EQ(finalized.dtm_sha256,
              hash::sha256(backend.finalized_dtm.data(),
                           backend.finalized_dtm.size()));
    const auto capture = std::ranges::find(
        backend.calls, std::string("capture-recording-checkpoint"));
    const auto finish = std::ranges::find(
        backend.calls, std::string("finalize-recording"));
    ASSERT_NE(capture, backend.calls.end());
    ASSERT_NE(finish, backend.calls.end());
    EXPECT_LT(capture, finish);
}

TEST(MovieService, BranchFailureLeavesReadOnlyPlaybackAndReservationIntact)
{
    TemporaryDirectory temp;
    const auto source = temp.path() / "source.dtm";
    Write(source, Dtm(false, 2));
    WorksetEpoch epoch(23);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = source});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.current_frame = 8;
    backend.observation.current_input_count = 2;
    backend.branch_result = MovieBackendResult::Failure(
        "branch refused", GuestIntegrity::Preserved);

    const auto result = service.StartRecording();

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Preserved);
    EXPECT_EQ(service.state(), MovieState::ReadOnlyPlayback);
    EXPECT_TRUE(backend.observation.playing);
    EXPECT_FALSE(backend.observation.recording);
    EXPECT_TRUE(backend.observation.read_only);
    EXPECT_TRUE(service.reservation());
    EXPECT_TRUE(reservations.held);
    EXPECT_FALSE(service.is_tainted());
    EXPECT_TRUE(service.StopPlayback().result.ok);
}

TEST(MovieService,
     SuccessfulBranchWithInvalidBackendStateIsCancelledAndRetired)
{
    TemporaryDirectory temp;
    const auto source = temp.path() / "source.dtm";
    Write(source, Dtm(false, 2));
    WorksetEpoch epoch(25);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = source});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.current_frame = 9;
    backend.observation.current_input_count = 2;
    backend.branch_observation_override = MovieBackendObservation{
        .result = MovieBackendResult::Success(),
        .playing = true,
        .read_only = true,
        .current_frame = 9,
        .current_input_count = 2};

    const auto result = service.StartRecording();

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Unknown);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_EQ(service.state(), MovieState::Inactive);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("cancel-recording")), 1);
}

TEST(MovieService,
     PrefixMismatchFailsBeforeFinalizationAndPublishesNoRecording)
{
    TemporaryDirectory temp;
    const auto source = temp.path() / "source.dtm";
    const auto output = temp.path() / "must-not-exist.dtm";
    Write(source, Dtm(false, 2));
    WorksetEpoch epoch(24);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = source});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.current_frame = 9;
    backend.observation.current_input_count = 2;
    const MovieStateSnapshot paused =
        service.ReconcilePausedState(epoch);
    ASSERT_TRUE(paused.result.ok) << paused.result.message;
    ASSERT_TRUE(service.StartRecording().result.ok);

    auto changed = Dtm(false, 3);
    changed[256] ^= 0xff;
    backend.capture_result = MovieBackendResult::Success();
    backend.capture_checkpoint = RecordingCheckpoint(std::move(changed), 13, 3);
    backend.finalize_result = MovieBackendResult::Success();
    backend.finalized_dtm = Dtm(false, 3);

    const auto result = service.FinalizeRecording({.dtm_path = output});

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.result.code, MovieServiceErrorCode::IntegrityFailure);
    EXPECT_EQ(result.result.integrity, GuestIntegrity::Unknown);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_EQ(service.state(), MovieState::Recording);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("capture-recording-checkpoint")), 1);
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("finalize-recording")), 0);
}

TEST(MovieService,
     PostFinalizeArtifactFailureRetiresRecordingWithoutCancellation)
{
    TemporaryDirectory temp;
    const auto source = temp.path() / "source.dtm";
    const auto output = temp.path() / "malformed-recording.dtm";
    Write(source, Dtm(false, 2));
    WorksetEpoch epoch(26);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });

    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = source});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    backend.observation.current_frame = 9;
    backend.observation.current_input_count = 2;
    ASSERT_TRUE(service.StartRecording().result.ok);

    backend.capture_result = MovieBackendResult::Success();
    backend.capture_checkpoint = RecordingCheckpoint(Dtm(false, 3), 13, 3);
    backend.finalize_result = MovieBackendResult::Success();
    backend.finalized_dtm = std::vector<std::uint8_t>(256, 0);

    const auto result = service.FinalizeRecording({.dtm_path = output});

    EXPECT_FALSE(result.result.ok);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_EQ(result.state, MovieState::Inactive);
    EXPECT_EQ(service.state(), MovieState::Inactive);
    EXPECT_FALSE(service.reservation());
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("finalize-recording")), 1);
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("cancel-recording")), 0);
    EXPECT_FALSE(service.CancelRecording().result.ok);
    EXPECT_EQ(std::ranges::count(
        backend.calls, std::string("cancel-recording")), 0);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(MovieService, SavestateRestoreHandshakeUsesExactActiveWorkset)
{
    WorksetEpoch epoch(4);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });
    MovieCheckpointMetadata movie;
    movie.mode = MovieCheckpointMode::ReadOnlyPlayback;
    movie.dtm_bytes = Dtm(true);
    movie.dtm_sha256 = hash::sha256(
        movie.dtm_bytes.data(), movie.dtm_bytes.size());
    movie.game_id = "GEAE8E";
    movie.cursor_known = true;
    movie.current_frame = 31;
    movie.current_input_count = 22;
    const SavestateMovieRestoreContext context{
        epoch, movie, true};

    ASSERT_TRUE(service.PrepareSavestateRestore(context).ok);
    ASSERT_TRUE(service.CommitSavestateRestore(context).ok);
    EXPECT_EQ(service.state(), MovieState::ReadOnlyPlayback);
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{
                  "prepare-savestate", "commit-savestate",
                  "acquire-pause-at-end"}));
    EXPECT_EQ(backend.observation_count, 1u);

    SavestateMovieRestoreContext stale = context;
    stale.workset_epoch = WorksetEpoch(5);
    EXPECT_FALSE(service.PrepareSavestateRestore(stale).ok);
}

TEST(MovieService, RecordingRestoreNeverAcquiresControllerReservation)
{
    WorksetEpoch epoch(27);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });
    MovieCheckpointMetadata movie = RecordingCheckpoint(Dtm(false, 2), 8, 2);
    movie.recording_workset_epoch = epoch;
    const SavestateMovieRestoreContext context{epoch, movie, false};

    ASSERT_TRUE(service.PrepareSavestateRestore(context).ok);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    ASSERT_TRUE(service.CommitSavestateRestore(context).ok);
    EXPECT_EQ(service.state(), MovieState::Recording);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);

    const MovieOperationReceipt cancelled = service.CancelRecording();
    EXPECT_TRUE(cancelled.result.ok) << cancelled.result.message;
    EXPECT_EQ(service.state(), MovieState::Inactive);
}

TEST(MovieService, RestoringRecordingReleasesPriorPlaybackReservation)
{
    TemporaryDirectory temp;
    const auto dtm = temp.path() / "root.dtm";
    Write(dtm, Dtm(false, 2));
    WorksetEpoch epoch(28);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });
    const auto prepared = service.PrepareReadOnlyPlayback({.dtm_path = dtm});
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    ASSERT_TRUE(service.StartPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    ASSERT_TRUE(service.reservation());

    MovieCheckpointMetadata movie = RecordingCheckpoint(Dtm(false, 2), 8, 2);
    movie.recording_workset_epoch = epoch;
    const SavestateMovieRestoreContext context{epoch, movie, false};
    ASSERT_TRUE(service.PrepareSavestateRestore(context).ok);
    ASSERT_TRUE(service.CommitSavestateRestore(context).ok);

    EXPECT_EQ(service.state(), MovieState::Recording);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_TRUE(service.CancelRecording().result.ok);
}

TEST(MovieService, RecordingRollbackReturnsToReservationFreeRecording)
{
    WorksetEpoch epoch(29);
    Backend backend;
    Reservations reservations;
    MovieService service(
        backend, reservations, [&] { return epoch; },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); },
        [] { return MovieServiceResult::Success(); });
    MovieCheckpointMetadata recording =
        RecordingCheckpoint(Dtm(false, 2), 8, 2);
    recording.recording_workset_epoch = epoch;
    const SavestateMovieRestoreContext recording_context{
        epoch, recording, false};
    ASSERT_TRUE(service.PrepareSavestateRestore(recording_context).ok);
    ASSERT_TRUE(service.CommitSavestateRestore(recording_context).ok);
    ASSERT_EQ(service.state(), MovieState::Recording);

    MovieCheckpointMetadata playback = recording;
    playback.mode = MovieCheckpointMode::ReadOnlyPlayback;
    playback.recording_workset_epoch = {};
    const SavestateMovieRestoreContext playback_context{
        epoch, playback, false};
    ASSERT_TRUE(service.PrepareSavestateRestore(playback_context).ok);
    ASSERT_TRUE(service.reservation());

    ASSERT_TRUE(service.RollbackSavestateRestore(playback_context).ok);
    EXPECT_EQ(service.state(), MovieState::Recording);
    EXPECT_FALSE(service.reservation());
    EXPECT_FALSE(reservations.held);
    EXPECT_TRUE(service.CancelRecording().result.ok);
}

} // namespace
