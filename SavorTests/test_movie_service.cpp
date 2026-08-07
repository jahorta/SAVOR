#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Movie/MovieService.h"
#include "Utils/Hash.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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

std::vector<std::uint8_t> Dtm(bool from_state = false)
{
    std::vector<std::uint8_t> bytes(256, 0);
    bytes[0] = 'D'; bytes[1] = 'T'; bytes[2] = 'M'; bytes[3] = 0x1a;
    bytes[4] = 'G'; bytes[5] = 'E'; bytes[6] = 'A';
    bytes[7] = 'E'; bytes[8] = '8'; bytes[9] = 'E';
    bytes[11] = 1;
    bytes[12] = from_state ? 1 : 0;
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
    MovieBackendResult StartPreparedReadOnlyMovie() override
    {
        calls.push_back("start-core");
        if (start_core_result.ok)
        {
            snapshot.activity = MovieActivity::ReadOnlyPlayback;
            snapshot.read_only = true;
        }
        return start_core_result;
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
        snapshot = {};
        return MovieBackendResult::Success();
    }
    MovieBackendResult BeginRecording() override
    {
        snapshot.activity = MovieActivity::Recording;
        snapshot.read_only = false;
        return MovieBackendResult::Success();
    }
    MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path&) override
    {
        return {MovieBackendResult::Failure("unused"), std::nullopt};
    }
    MovieBackendResult CancelRecording() noexcept override
    {
        snapshot = {};
        return MovieBackendResult::Success();
    }
    MovieSnapshot Snapshot() const override { return snapshot; }
    MovieCheckpointBackendResult CaptureRecordingCheckpoint() override
    {
        return {MovieBackendResult::Failure("unused"), {}};
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
            snapshot.activity = context.movie->mode ==
                    MovieCheckpointMode::Recording
                ? MovieActivity::Recording
                : MovieActivity::ReadOnlyPlayback;
            snapshot.read_only = context.movie->mode ==
                MovieCheckpointMode::ReadOnlyPlayback;
            snapshot.current_frame = context.movie->current_frame;
            snapshot.current_input_count =
                context.movie->current_input_count;
        }
        else
        {
            snapshot = {};
        }
        return restore_commit_result;
    }
    MovieBackendResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext&) noexcept override
    {
        calls.push_back("rollback-savestate");
        return restore_rollback_result;
    }

    std::vector<std::string> calls;
    std::filesystem::path prepared;
    std::optional<std::filesystem::path> startup;
    std::optional<SavestateMovieRestoreContext> last_restore;
    MovieSnapshot snapshot;
    MovieBackendResult prepare_result = MovieBackendResult::Success();
    MovieBackendResult stop_core_result = MovieBackendResult::Success();
    MovieBackendResult start_core_result = MovieBackendResult::Success();
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

TEST(MovieService, PlaybackStopsThenStartsCoreAndPreservesWorksetEpoch)
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
                  "prepare", "stop-core", "start-core"}));
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
    ASSERT_TRUE(prepared.result.ok) << prepared.result.message;
    const MovieOperationReceipt result =
        service.StartPreparedReadOnlyPlayback(prepared.preparation);

    EXPECT_FALSE(result.result.ok);
    EXPECT_EQ(result.workset_epoch, epoch);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_TRUE(reservations.held);
    EXPECT_FALSE(service.AbandonPreparedReadOnlyPlayback(
        prepared.preparation).result.ok);
    EXPECT_FALSE(reservations.held);
}

TEST(MovieService, AbandoningStoppedPreparedCoreReleasesResourcesAndTaints)
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

    EXPECT_FALSE(abandoned.result.ok);
    EXPECT_EQ(abandoned.result.integrity, GuestIntegrity::Unknown);
    EXPECT_EQ(abandoned.workset_epoch, epoch);
    EXPECT_TRUE(service.is_tainted());
    EXPECT_FALSE(reservations.held);
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{
                  "prepare", "stop-core", "discard-prepared"}));
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
    EXPECT_EQ(service.activity(), MovieActivity::ReadOnlyPlayback);
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{
                  "prepare-savestate", "commit-savestate"}));

    SavestateMovieRestoreContext stale = context;
    stale.workset_epoch = WorksetEpoch(5);
    EXPECT_FALSE(service.PrepareSavestateRestore(stale).ok);
}

} // namespace
