#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Movie/MovieService.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;

class TemporaryMovieDirectory final
{
public:
    TemporaryMovieDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-movie-service-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryMovieDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> MakeDtm(bool from_state = false)
{
    std::vector<std::uint8_t> bytes(256, 0);
    bytes[0] = 'D';
    bytes[1] = 'T';
    bytes[2] = 'M';
    bytes[3] = 0x1a;
    bytes[4] = 'G';
    bytes[5] = 'E';
    bytes[6] = 'A';
    bytes[7] = 'E';
    bytes[8] = '8';
    bytes[9] = 'E';
    bytes[11] = 1;
    bytes[12] = from_state ? 1 : 0;
    return bytes;
}

void WriteMovieBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}

StateCompatibilityToken MovieCompatibility()
{
    return {
        .game_id = "GEAE8E",
        .iso_sha256 = std::string(64, 'c'),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "slice4-test",
    };
}

class FakeMovieStateBackend final : public IStateBackendPort
{
public:
    explicit FakeMovieStateBackend(std::vector<std::string>& calls)
        : calls_(calls)
    {
    }

    StateBackendResult Boot(const StateBootRequest& request) override
    {
        calls_.push_back("state.boot");
        last_boot = request;
        return boot_result;
    }

    StateBackendResult Reboot(const StateBootRequest& request) override
    {
        calls_.push_back("state.reboot");
        last_boot = request;
        return reboot_result;
    }

    StateBackendResult Shutdown() noexcept override
    {
        calls_.push_back("state.shutdown");
        return StateBackendResult::Success();
    }

    StateCompatibilityToken CurrentCompatibility() const override
    {
        return MovieCompatibility();
    }

    StateBackendBufferResult SaveStateBuffer() override
    {
        calls_.push_back("state.save-buffer");
        return {StateBackendResult::Success(), state_bytes};
    }

    StateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override
    {
        calls_.push_back("state.restore-buffer");
        restored_bytes = bytes;
        return restore_result;
    }

    StateBackendResult SaveStateFile(
        const std::filesystem::path& path) override
    {
        calls_.push_back("state.save-file");
        WriteMovieBytes(path, state_bytes);
        return StateBackendResult::Success();
    }

    StateBackendResult RestoreStateFile(
        const std::filesystem::path&) override
    {
        calls_.push_back("state.restore-file");
        return restore_result;
    }

    std::vector<std::string>& calls_;
    StateBackendResult boot_result = StateBackendResult::Success();
    StateBackendResult reboot_result = StateBackendResult::Success();
    StateBackendResult restore_result = StateBackendResult::Success();
    StateBootRequest last_boot;
    std::vector<std::uint8_t> state_bytes{7, 7, 7, 7};
    std::vector<std::uint8_t> restored_bytes;
};

class FakeMovieBackend final : public IMovieBackendPort
{
public:
    explicit FakeMovieBackend(std::vector<std::string>& calls)
        : calls_(calls)
    {
    }

    MoviePlaybackPrepareResult PrepareReadOnlyPlaybackBeforeBoot(
        const std::filesystem::path& path) override
    {
        calls_.push_back("movie.prepare-playback");
        prepared_path = path;
        return {prepare_playback_result, startup_savestate};
    }

    MovieBackendResult StopMovie() noexcept override
    {
        calls_.push_back("movie.stop");
        if (stop_result.ok)
            snapshot.activity = MovieActivity::Inactive;
        return stop_result;
    }

    MovieBackendResult BeginRecording() override
    {
        calls_.push_back("movie.begin-recording");
        if (begin_result.ok)
        {
            snapshot.activity = MovieActivity::Recording;
            snapshot.read_only = false;
        }
        return begin_result;
    }

    MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& path) override
    {
        calls_.push_back("movie.finalize-recording");
        if (finalize_result.ok)
        {
            WriteMovieBytes(path, finalized_dtm);
            snapshot.activity = MovieActivity::Inactive;
        }
        return {finalize_result, finalized_starting_state};
    }

    MovieBackendResult CancelRecording() noexcept override
    {
        calls_.push_back("movie.cancel-recording");
        if (cancel_result.ok)
            snapshot.activity = MovieActivity::Inactive;
        return cancel_result;
    }

    MovieSnapshot Snapshot() const override
    {
        return snapshot;
    }

    MovieCheckpointBackendResult CaptureRecordingCheckpoint() override
    {
        calls_.push_back("movie.capture-checkpoint");
        return {
            capture_result,
            MovieCheckpointMetadata{
                .mode = MovieCheckpointMode::Recording,
                .dtm_bytes = recording_checkpoint,
                .current_frame = snapshot.current_frame,
                .current_input_count = snapshot.current_input_count,
            }};
    }

    MovieBackendResult PrepareStateReplacement(
        const StateReplacementContext& context) override
    {
        calls_.push_back("movie.prepare-state");
        pending_context = context;
        return prepare_state_result;
    }

    MovieBackendResult CommitStateReplacement(
        const StateReplacementContext& context) override
    {
        calls_.push_back("movie.commit-state");
        if (commit_state_result.ok)
        {
            if (!context.movie.has_value())
            {
                snapshot.activity = MovieActivity::Inactive;
                snapshot.read_only = true;
            }
            else if (
                context.movie->mode ==
                MovieCheckpointMode::ReadOnlyPlayback)
            {
                snapshot.activity = MovieActivity::ReadOnlyPlayback;
                snapshot.read_only = true;
            }
            else
            {
                snapshot.activity = MovieActivity::Recording;
                snapshot.read_only = false;
            }
        }
        return commit_state_result;
    }

    MovieBackendResult RollbackStateReplacement(
        const StateReplacementContext&) noexcept override
    {
        calls_.push_back("movie.rollback-state");
        return rollback_state_result;
    }

    std::vector<std::string>& calls_;
    MovieSnapshot snapshot;
    std::filesystem::path prepared_path;
    std::optional<std::filesystem::path> startup_savestate;
    std::optional<std::filesystem::path> finalized_starting_state;
    std::optional<StateReplacementContext> pending_context;
    std::vector<std::uint8_t> recording_checkpoint = MakeDtm(true);
    std::vector<std::uint8_t> finalized_dtm = MakeDtm(false);
    MovieBackendResult prepare_playback_result = MovieBackendResult::Success();
    MovieBackendResult stop_result = MovieBackendResult::Success();
    MovieBackendResult begin_result = MovieBackendResult::Success();
    MovieBackendResult finalize_result = MovieBackendResult::Success();
    MovieBackendResult cancel_result = MovieBackendResult::Success();
    MovieBackendResult capture_result = MovieBackendResult::Success();
    MovieBackendResult prepare_state_result = MovieBackendResult::Success();
    MovieBackendResult commit_state_result = MovieBackendResult::Success();
    MovieBackendResult rollback_state_result = MovieBackendResult::Success();
};

class FakeMovieReservations final : public IMovieInputReservationPort
{
public:
    explicit FakeMovieReservations(std::vector<std::string>& calls)
        : calls_(calls)
    {
    }

    MovieInputReservationReceipt
    AcquireUnsuspendableMovieReservation() override
    {
        calls_.push_back("reservation.acquire-unsuspendable");
        if (!acquire_result.ok)
            return {acquire_result, {}};
        held = MovieReservationId(next_id++);
        return {MovieServiceResult::Success(), held};
    }

    MovieServiceResult ReleaseMovieReservation(
        MovieReservationId reservation) noexcept override
    {
        calls_.push_back("reservation.release");
        if (!release_result.ok)
            return release_result;
        if (reservation != held)
        {
            return MovieServiceResult::Failure(
                MovieServiceErrorCode::ReservationFailure,
                "wrong reservation");
        }
        held = {};
        return MovieServiceResult::Success();
    }

    std::vector<std::string>& calls_;
    MovieServiceResult acquire_result = MovieServiceResult::Success();
    MovieServiceResult release_result = MovieServiceResult::Success();
    MovieReservationId held;
    std::uint64_t next_id = 1;
};

struct MovieHarness
{
    std::vector<std::string> calls;
    FakeMovieStateBackend state_backend{calls};
    StateService state{state_backend};
    FakeMovieBackend movie_backend{calls};
    FakeMovieReservations reservations{calls};
    MovieService movie{movie_backend, reservations, state};

    MovieHarness()
    {
        EXPECT_TRUE(movie.RegisterForStateReplacement().ok);
    }
};

TEST(MovieService, PlaybackStagesDtmBeforeBootAndHoldsReservation)
{
    TemporaryMovieDirectory temp;
    const std::filesystem::path dtm = temp.path() / "movie.dtm";
    WriteMovieBytes(dtm, MakeDtm(false));
    MovieHarness harness;

    const MovieOperationReceipt started =
        harness.movie.StartReadOnlyPlayback({.dtm_path = dtm});

    ASSERT_TRUE(started.result.ok) << started.result.message;
    EXPECT_EQ(started.activity, MovieActivity::ReadOnlyPlayback);
    EXPECT_EQ(started.state_epoch, StateEpoch(1));
    EXPECT_TRUE(started.reservation);
    EXPECT_EQ(
        harness.calls,
        (std::vector<std::string>{
            "reservation.acquire-unsuspendable",
            "movie.prepare-playback",
            "movie.prepare-state",
            "state.boot",
            "movie.commit-state"}));
    ASSERT_TRUE(harness.state_backend.last_boot.movie.has_value());
    EXPECT_EQ(
        harness.state_backend.last_boot.movie->dtm_sha256,
        hash::sha256_of_file(dtm.string()));

    const MovieOperationReceipt stopped = harness.movie.StopPlayback();
    ASSERT_TRUE(stopped.result.ok);
    EXPECT_FALSE(harness.reservations.held);
}

TEST(MovieService, PlaybackFromSavestatePropagatesStartupState)
{
    TemporaryMovieDirectory temp;
    const std::filesystem::path dtm = temp.path() / "movie.dtm";
    const std::filesystem::path startup = temp.path() / "movie.dtm.sav";
    WriteMovieBytes(dtm, MakeDtm(true));
    WriteMovieBytes(startup, {1, 2, 3});
    MovieHarness harness;
    harness.movie_backend.startup_savestate = startup;

    const MovieOperationReceipt started =
        harness.movie.StartReadOnlyPlayback({.dtm_path = dtm});

    ASSERT_TRUE(started.result.ok) << started.result.message;
    EXPECT_EQ(started.starting_savestate, startup);
    EXPECT_EQ(harness.state_backend.last_boot.startup_savestate, startup);
}

TEST(MovieService, PostOpenPlaybackUsesStateServiceReboot)
{
    TemporaryMovieDirectory temp;
    const std::filesystem::path dtm = temp.path() / "movie.dtm";
    WriteMovieBytes(dtm, MakeDtm(false));
    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    harness.calls.clear();

    const MovieOperationReceipt started =
        harness.movie.StartReadOnlyPlayback({.dtm_path = dtm});

    ASSERT_TRUE(started.result.ok) << started.result.message;
    EXPECT_EQ(
        harness.calls,
        (std::vector<std::string>{
            "reservation.acquire-unsuspendable",
            "movie.prepare-playback",
            "movie.prepare-state",
            "state.reboot",
            "movie.commit-state"}));
    EXPECT_EQ(
        std::count(
            harness.calls.begin(),
            harness.calls.end(),
            "state.boot"),
        0);
}

TEST(MovieService, ExternalUnknownCursorRecordsDolphinsRestoredPosition)
{
    TemporaryMovieDirectory temp;
    const std::filesystem::path state_path =
        temp.path() / "external.sav";
    const std::filesystem::path dtm_path =
        std::filesystem::path(state_path.string() + ".dtm");
    WriteMovieBytes(state_path, {1, 2, 3, 4});
    WriteMovieBytes(dtm_path, MakeDtm(true));

    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    const StateFileArtifactReceipt imported =
        harness.state.ImportFileArtifact({
            .path = state_path,
            .expected_sha256 =
                hash::sha256_of_file(state_path.string()),
            .compatibility = MovieCompatibility(),
            .movie_mode =
                ExternalMovieImportMode::ReadOnlyPlayback,
            .dtm_path = dtm_path,
            .expected_dtm_sha256 =
                hash::sha256_of_file(dtm_path.string()),
        });
    ASSERT_TRUE(imported.result.ok) << imported.result.message;
    ASSERT_TRUE(imported.movie.has_value());
    EXPECT_FALSE(imported.movie->cursor_known);

    harness.movie_backend.snapshot.current_frame = 321;
    harness.movie_backend.snapshot.current_input_count = 654;
    const StateOperationReceipt restored =
        harness.state.RestoreFileArtifact(imported.artifact);

    ASSERT_TRUE(restored.result.ok) << restored.result.message;
    const MovieCheckpointReceipt reconciled =
        harness.movie.CaptureCheckpoint();
    ASSERT_TRUE(reconciled.result.ok) << reconciled.result.message;
    ASSERT_TRUE(reconciled.checkpoint.has_value());
    EXPECT_TRUE(reconciled.checkpoint->cursor_known);
    EXPECT_EQ(reconciled.checkpoint->current_frame, 321);
    EXPECT_EQ(reconciled.checkpoint->current_input_count, 654);
}

TEST(MovieService, KnownInternalCursorMismatchFailsClosed)
{
    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    MovieCheckpointMetadata movie{
        .mode = MovieCheckpointMode::ReadOnlyPlayback,
        .dtm_bytes = MakeDtm(false),
        .current_frame = 42,
        .current_input_count = 99,
        .cursor_known = true,
    };
    const StateHandleReceipt captured =
        harness.state.CaptureMemoryHandle({.movie = movie});
    ASSERT_TRUE(captured.result.ok) << captured.result.message;

    harness.movie_backend.snapshot.current_frame = 41;
    harness.movie_backend.snapshot.current_input_count = 99;
    const StateOperationReceipt restored =
        harness.state.RestoreMemoryHandle(captured.handle);

    EXPECT_FALSE(restored.result.ok);
    EXPECT_EQ(
        restored.result.integrity,
        StateIntegrity::Unknown);
    EXPECT_NE(
        restored.result.message.find("cursor"),
        std::string::npos);
}

TEST(MovieService, FailedBootRollsBackMovieAndReservation)
{
    TemporaryMovieDirectory temp;
    const std::filesystem::path dtm = temp.path() / "movie.dtm";
    WriteMovieBytes(dtm, MakeDtm(false));
    MovieHarness harness;
    harness.state_backend.boot_result =
        StateBackendResult::Failure("boot failed");

    const MovieOperationReceipt failed =
        harness.movie.StartReadOnlyPlayback({.dtm_path = dtm});

    EXPECT_FALSE(failed.result.ok);
    EXPECT_FALSE(harness.reservations.held);
    EXPECT_EQ(harness.movie.activity(), MovieActivity::Inactive);
    EXPECT_NE(
        std::find(
            harness.calls.begin(),
            harness.calls.end(),
            "movie.rollback-state"),
        harness.calls.end());
    EXPECT_NE(
        std::find(
            harness.calls.begin(),
            harness.calls.end(),
            "movie.stop"),
        harness.calls.end());
}

TEST(MovieService, RecordingCheckpointSupportsSameSessionRewind)
{
    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    harness.calls.clear();

    const MovieOperationReceipt started = harness.movie.StartRecording();
    ASSERT_TRUE(started.result.ok);
    harness.movie_backend.snapshot.current_frame = 42;
    harness.movie_backend.snapshot.current_input_count = 99;
    const MovieCheckpointReceipt movie_checkpoint =
        harness.movie.CaptureCheckpoint();
    ASSERT_TRUE(movie_checkpoint.result.ok);
    ASSERT_TRUE(movie_checkpoint.checkpoint.has_value());
    EXPECT_EQ(
        movie_checkpoint.checkpoint->mode,
        MovieCheckpointMode::Recording);

    const StateHandleReceipt state_checkpoint =
        harness.state.CaptureMemoryHandle({
            .movie = movie_checkpoint.checkpoint,
        });
    ASSERT_TRUE(state_checkpoint.result.ok) << state_checkpoint.result.message;
    const StateOperationReceipt restored =
        harness.state.RestoreMemoryHandle(state_checkpoint.handle);
    ASSERT_TRUE(restored.result.ok) << restored.result.message;
    EXPECT_EQ(harness.movie.activity(), MovieActivity::Recording);
    EXPECT_TRUE(harness.reservations.held);
}

TEST(MovieService, RecordingFinalizationPublishesTypedArtifact)
{
    TemporaryMovieDirectory temp;
    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    ASSERT_TRUE(harness.movie.StartRecording().result.ok);
    const std::filesystem::path output = temp.path() / "recording.dtm";

    const MovieOperationReceipt finalized =
        harness.movie.FinalizeRecording({.dtm_path = output});

    ASSERT_TRUE(finalized.result.ok) << finalized.result.message;
    EXPECT_EQ(finalized.activity, MovieActivity::Inactive);
    EXPECT_EQ(finalized.artifact_path, output);
    EXPECT_EQ(finalized.dtm_sha256, hash::sha256_of_file(output.string()));
    EXPECT_FALSE(harness.reservations.held);

    const MovieOperationReceipt second =
        harness.movie.StartRecording();
    ASSERT_TRUE(second.result.ok);
    const MovieOperationReceipt overwrite =
        harness.movie.FinalizeRecording({.dtm_path = output});
    EXPECT_FALSE(overwrite.result.ok);
    EXPECT_EQ(
        overwrite.result.code,
        MovieServiceErrorCode::InvalidArgument);
    ASSERT_TRUE(harness.movie.CancelRecording().result.ok);
}

TEST(MovieService, ParticipantRejectsColdRecordingRestoreBeforeBackend)
{
    MovieHarness harness;
    ASSERT_TRUE(harness.state.Boot().result.ok);
    StateReplacementContext context{
        .kind = StateReplacementKind::RestoreFileArtifact,
        .origin_epoch = StateEpoch(1),
        .candidate_epoch = StateEpoch(2),
        .compatibility = MovieCompatibility(),
        .movie = MovieCheckpointMetadata{
            .mode = MovieCheckpointMode::Recording,
            .dtm_bytes = MakeDtm(true),
        },
        .external_artifact = true,
    };
    harness.calls.clear();

    const StateServiceResult rejected =
        harness.movie.PrepareStateReplacement(context);

    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.code, StateServiceErrorCode::Unsupported);
    EXPECT_TRUE(harness.calls.empty());
}

TEST(
    MovieServiceOwnership,
    RejectsOffActorOperationsWithoutBackendStateOrReservationCalls)
{
    MovieHarness harness;
    MovieServiceResult registered;
    MovieOperationReceipt playback;
    MovieOperationReceipt stopped;
    MovieOperationReceipt recording;
    MovieOperationReceipt finalized;
    MovieOperationReceipt cancelled;
    MovieCheckpointReceipt checkpoint;
    MovieSnapshot snapshot;
    StateServiceResult prepared;
    StateServiceResult committed;
    StateServiceResult rolled_back;
    StateReplacementContext context{
        .kind = StateReplacementKind::RestoreMemoryHandle,
        .origin_epoch = StateEpoch(1),
        .candidate_epoch = StateEpoch(2),
        .compatibility = MovieCompatibility(),
    };

    std::thread other([&] {
        registered = harness.movie.RegisterForStateReplacement();
        playback = harness.movie.StartReadOnlyPlayback(
            {.dtm_path = "off-thread.dtm"});
        stopped = harness.movie.StopPlayback();
        recording = harness.movie.StartRecording();
        finalized = harness.movie.FinalizeRecording(
            {.dtm_path = "off-thread-recording.dtm"});
        cancelled = harness.movie.CancelRecording();
        checkpoint = harness.movie.CaptureCheckpoint();
        snapshot = harness.movie.snapshot();
        prepared = harness.movie.PrepareStateReplacement(context);
        committed = harness.movie.CommitStateReplacement(context);
        rolled_back =
            harness.movie.RollbackStateReplacement(context);
    });
    other.join();

    EXPECT_FALSE(registered.ok);
    EXPECT_EQ(registered.code, MovieServiceErrorCode::InvalidState);
    EXPECT_FALSE(playback.result.ok);
    EXPECT_FALSE(stopped.result.ok);
    EXPECT_FALSE(recording.result.ok);
    EXPECT_FALSE(finalized.result.ok);
    EXPECT_FALSE(cancelled.result.ok);
    EXPECT_FALSE(checkpoint.result.ok);
    EXPECT_EQ(snapshot.activity, MovieActivity::Inactive);
    EXPECT_FALSE(prepared.ok);
    EXPECT_EQ(
        prepared.code,
        StateServiceErrorCode::ParticipantFailure);
    EXPECT_FALSE(committed.ok);
    EXPECT_FALSE(rolled_back.ok);
    EXPECT_TRUE(harness.calls.empty());
    EXPECT_FALSE(harness.reservations.held);
}

} // namespace
