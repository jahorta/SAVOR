#include <gtest/gtest.h>

#include "Runner/Runtime/Services/State/StateService.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-state-service-" + std::to_string(stamp));
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

std::vector<std::uint8_t> DtmBytes(bool from_state = false)
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

void WriteBytes(
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

StateCompatibilityToken Compatibility(char iso_fill = 'a')
{
    return {
        .game_id = "GEAE8E",
        .iso_sha256 = std::string(64, iso_fill),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "slice4-test",
    };
}

class FakeStateBackend final : public IStateBackendPort
{
public:
    StateBackendResult Boot(const StateBootRequest&) override
    {
        calls.push_back("boot");
        return boot_result;
    }

    StateBackendResult Reboot(const StateBootRequest&) override
    {
        calls.push_back("reboot");
        return reboot_result;
    }

    StateBackendResult Shutdown() noexcept override
    {
        calls.push_back("shutdown");
        return shutdown_result;
    }

    StateCompatibilityToken CurrentCompatibility() const override
    {
        return compatibility;
    }

    StateBackendBufferResult SaveStateBuffer() override
    {
        calls.push_back("save-buffer");
        return {save_buffer_result, next_buffer};
    }

    StateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override
    {
        calls.push_back("restore-buffer");
        restored_buffer = bytes;
        return restore_buffer_result;
    }

    StateBackendResult SaveStateFile(
        const std::filesystem::path& path) override
    {
        calls.push_back("save-file");
        if (save_file_result.ok)
            WriteBytes(path, next_file);
        return save_file_result;
    }

    StateBackendResult RestoreStateFile(
        const std::filesystem::path& path) override
    {
        calls.push_back("restore-file");
        restored_file = path;
        return restore_file_result;
    }

    StateCompatibilityToken compatibility = Compatibility();
    StateBackendResult boot_result = StateBackendResult::Success();
    StateBackendResult reboot_result = StateBackendResult::Success();
    StateBackendResult shutdown_result = StateBackendResult::Success();
    StateBackendResult save_buffer_result = StateBackendResult::Success();
    StateBackendResult restore_buffer_result = StateBackendResult::Success();
    StateBackendResult save_file_result = StateBackendResult::Success();
    StateBackendResult restore_file_result = StateBackendResult::Success();
    std::vector<std::uint8_t> next_buffer{1, 2, 3, 4};
    std::vector<std::uint8_t> next_file{5, 6, 7, 8};
    std::vector<std::uint8_t> restored_buffer;
    std::filesystem::path restored_file;
    std::vector<std::string> calls;
};

class RecordingParticipant final : public IStateReplacementParticipant
{
public:
    StateServiceResult PrepareStateReplacement(
        const StateReplacementContext& context) override
    {
        calls.push_back(name + ".prepare");
        contexts.push_back(context);
        return prepare_result;
    }

    StateServiceResult CommitStateReplacement(
        const StateReplacementContext& context) override
    {
        calls.push_back(name + ".commit");
        contexts.push_back(context);
        return commit_result;
    }

    StateServiceResult RollbackStateReplacement(
        const StateReplacementContext& context) noexcept override
    {
        calls.push_back(name + ".rollback");
        contexts.push_back(context);
        return rollback_result;
    }

    std::string name = "participant";
    StateServiceResult prepare_result = StateServiceResult::Success();
    StateServiceResult commit_result = StateServiceResult::Success();
    StateServiceResult rollback_result = StateServiceResult::Success();
    std::vector<std::string> calls;
    std::vector<StateReplacementContext> contexts;
};

TEST(StateService, OwnsEpochAndTransactionsParticipants)
{
    FakeStateBackend backend;
    RecordingParticipant participant;
    StateService service(backend);
    ASSERT_TRUE(service.RegisterParticipant(participant).ok);

    const StateOperationReceipt boot = service.Boot();
    ASSERT_TRUE(boot.result.ok) << boot.result.message;
    EXPECT_EQ(boot.origin_epoch, StateEpoch{});
    EXPECT_EQ(boot.resulting_epoch, StateEpoch(1));
    EXPECT_EQ(service.current_epoch(), StateEpoch(1));
    EXPECT_EQ(service.compatibility(), Compatibility());
    EXPECT_EQ(
        participant.calls,
        (std::vector<std::string>{
            "participant.prepare",
            "participant.commit"}));

    const StateOperationReceipt reboot = service.Reboot();
    ASSERT_TRUE(reboot.result.ok) << reboot.result.message;
    EXPECT_EQ(reboot.origin_epoch, StateEpoch(1));
    EXPECT_EQ(reboot.resulting_epoch, StateEpoch(2));
    EXPECT_EQ(service.session_generation(), 2u);
}

TEST(StateService, RecoverableBackendFailureRollsBackWithoutAdvancing)
{
    FakeStateBackend backend;
    RecordingParticipant first;
    first.name = "first";
    RecordingParticipant second;
    second.name = "second";
    StateService service(backend);
    ASSERT_TRUE(service.RegisterParticipant(first).ok);
    ASSERT_TRUE(service.RegisterParticipant(second).ok);
    ASSERT_TRUE(service.Boot().result.ok);
    first.calls.clear();
    second.calls.clear();

    backend.reboot_result =
        StateBackendResult::Failure("reboot rejected");
    const StateOperationReceipt failed = service.Reboot();

    EXPECT_FALSE(failed.result.ok);
    EXPECT_EQ(failed.result.code, StateServiceErrorCode::BackendFailure);
    EXPECT_EQ(service.current_epoch(), StateEpoch(1));
    EXPECT_FALSE(service.is_tainted());
    EXPECT_EQ(
        first.calls,
        (std::vector<std::string>{"first.prepare", "first.rollback"}));
    EXPECT_EQ(
        second.calls,
        (std::vector<std::string>{"second.prepare", "second.rollback"}));
}

TEST(StateService, CommitFailureAdvancesEpochAndTaints)
{
    FakeStateBackend backend;
    RecordingParticipant participant;
    StateService service(backend);
    ASSERT_TRUE(service.RegisterParticipant(participant).ok);
    ASSERT_TRUE(service.Boot().result.ok);
    participant.commit_result = StateServiceResult::Failure(
        StateServiceErrorCode::ParticipantFailure,
        "commit failed");

    const StateOperationReceipt failed = service.Reboot();

    EXPECT_FALSE(failed.result.ok);
    EXPECT_EQ(failed.resulting_epoch, StateEpoch(2));
    EXPECT_EQ(service.current_epoch(), StateEpoch(2));
    EXPECT_TRUE(service.is_tainted());
}

TEST(StateService, MemoryHandlesAreImmutableAndBounded)
{
    FakeStateBackend backend;
    StateService service(
        backend,
        StateServiceLimits{
            .maximum_memory_handles = 2,
            .maximum_handle_bytes = 16,
            .maximum_total_memory_bytes = 8,
        });
    ASSERT_TRUE(service.Boot().result.ok);

    backend.next_buffer = {1, 2, 3, 4};
    const StateHandleReceipt first = service.CaptureMemoryHandle();
    ASSERT_TRUE(first.result.ok);
    backend.next_buffer = {9, 9, 9, 9};
    const StateHandleReceipt second = service.CaptureMemoryHandle();
    ASSERT_TRUE(second.result.ok);
    EXPECT_EQ(service.memory_bytes_in_use(), 8u);

    const StateHandleReceipt full = service.CaptureMemoryHandle();
    EXPECT_FALSE(full.result.ok);
    EXPECT_EQ(full.result.code, StateServiceErrorCode::CapacityExceeded);

    const StateOperationReceipt restored =
        service.RestoreMemoryHandle(first.handle);
    ASSERT_TRUE(restored.result.ok);
    EXPECT_EQ(backend.restored_buffer, (std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(restored.resulting_epoch, StateEpoch(2));

    ASSERT_TRUE(service.ReleaseMemoryHandle(first.handle).ok);
    EXPECT_EQ(service.memory_bytes_in_use(), 4u);
}

TEST(StateService, SameSessionRecordingRewindIsAllowedButColdRewindIsNot)
{
    FakeStateBackend backend;
    StateService service(backend);
    ASSERT_TRUE(service.Boot().result.ok);

    MovieCheckpointMetadata recording{
        .mode = MovieCheckpointMode::Recording,
        .dtm_bytes = DtmBytes(true),
        .starts_from_savestate = true,
    };
    const StateHandleReceipt handle =
        service.CaptureMemoryHandle({.movie = recording});
    ASSERT_TRUE(handle.result.ok) << handle.result.message;
    ASSERT_TRUE(service.RestoreMemoryHandle(handle.handle).result.ok);

    ASSERT_TRUE(service.Reboot().result.ok);
    const StateOperationReceipt cold =
        service.RestoreMemoryHandle(handle.handle);
    EXPECT_FALSE(cold.result.ok);
    EXPECT_EQ(cold.result.code, StateServiceErrorCode::Unsupported);
}

TEST(StateService, PublishesImmutableStateAndExactDtmPair)
{
    TemporaryDirectory temp;
    FakeStateBackend backend;
    RecordingParticipant participant;
    StateService service(backend);
    ASSERT_TRUE(service.RegisterParticipant(participant).ok);
    ASSERT_TRUE(service.Boot().result.ok);
    const std::filesystem::path state = temp.path() / "checkpoint.sav";
    const std::filesystem::path source_dtm = temp.path() / "source.dtm";
    WriteBytes(source_dtm, DtmBytes());
    MovieCheckpointMetadata movie{
        .mode = MovieCheckpointMode::ReadOnlyPlayback,
        .dtm_bytes = DtmBytes(),
        .dtm_path = source_dtm,
    };

    const StateFileArtifactReceipt saved =
        service.CaptureFileArtifact({
            .path = state,
            .movie = movie,
            .lineage = {.edge = "checkpoint", .producer = "test"},
        });
    ASSERT_TRUE(saved.result.ok) << saved.result.message;
    EXPECT_TRUE(std::filesystem::is_regular_file(state));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        std::filesystem::path(state.string() + ".dtm")));
    EXPECT_EQ(saved.movie->dtm_sha256, hash::sha256(
        movie.dtm_bytes.data(), movie.dtm_bytes.size()));
    EXPECT_EQ(
        saved.movie->dtm_path,
        std::filesystem::path(state.string() + ".dtm"));

    const StateFileArtifactReceipt overwrite =
        service.CaptureFileArtifact({.path = state});
    EXPECT_FALSE(overwrite.result.ok);

    WriteBytes(source_dtm, {0xff});
    participant.contexts.clear();
    const StateOperationReceipt restored =
        service.RestoreFileArtifact(saved.artifact);
    ASSERT_TRUE(restored.result.ok) << restored.result.message;
    EXPECT_EQ(backend.restored_file, state);
    ASSERT_FALSE(participant.contexts.empty());
    ASSERT_TRUE(participant.contexts.back().movie.has_value());
    EXPECT_EQ(
        participant.contexts.back().movie->dtm_path,
        std::filesystem::path(state.string() + ".dtm"));

    WriteBytes(state, {0xff});
    const StateOperationReceipt changed =
        service.RestoreFileArtifact(saved.artifact);
    EXPECT_FALSE(changed.result.ok);
    EXPECT_EQ(changed.result.code, StateServiceErrorCode::ArtifactFailure);
}

TEST(StateService, RecordingFileCaptureIsRejectedBeforeBackendMutation)
{
    TemporaryDirectory temp;
    FakeStateBackend backend;
    StateService service(backend);
    ASSERT_TRUE(service.Boot().result.ok);
    const std::size_t save_calls_before = std::ranges::count(
        backend.calls,
        std::string{"save-file"});

    const StateFileArtifactReceipt rejected =
        service.CaptureFileArtifact({
            .path = temp.path() / "recording.sav",
            .movie = MovieCheckpointMetadata{
                .mode = MovieCheckpointMode::Recording,
                .dtm_bytes = DtmBytes(true),
                .starts_from_savestate = true,
            },
        });

    EXPECT_FALSE(rejected.result.ok);
    EXPECT_EQ(rejected.result.code, StateServiceErrorCode::Unsupported);
    EXPECT_EQ(
        std::ranges::count(backend.calls, std::string{"save-file"}),
        save_calls_before);
}

TEST(StateService, ExternalImportRequiresExplicitReadOnlyMovieMode)
{
    TemporaryDirectory temp;
    FakeStateBackend backend;
    StateService service(backend);
    ASSERT_TRUE(service.Boot().result.ok);

    const std::filesystem::path state = temp.path() / "external.sav";
    const std::filesystem::path dtm =
        std::filesystem::path(state.string() + ".dtm");
    WriteBytes(state, {1, 3, 3, 7});
    WriteBytes(dtm, DtmBytes());
    const std::string state_hash = hash::sha256_of_file(state.string());
    const std::string dtm_hash = hash::sha256_of_file(dtm.string());

    StateFileImportRequest request{
        .path = state,
        .expected_sha256 = state_hash,
        .compatibility = Compatibility(),
    };
    const StateFileArtifactReceipt unspecified =
        service.ImportFileArtifact(request);
    EXPECT_FALSE(unspecified.result.ok);
    EXPECT_EQ(
        unspecified.result.code,
        StateServiceErrorCode::InvalidArgument);

    request.movie_mode = ExternalMovieImportMode::Recording;
    const StateFileArtifactReceipt recording =
        service.ImportFileArtifact(request);
    EXPECT_FALSE(recording.result.ok);
    EXPECT_EQ(recording.result.code, StateServiceErrorCode::Unsupported);

    request.movie_mode = ExternalMovieImportMode::ReadOnlyPlayback;
    request.dtm_path = dtm;
    request.expected_dtm_sha256 = dtm_hash;
    const StateFileArtifactReceipt playback =
        service.ImportFileArtifact(request);
    ASSERT_TRUE(playback.result.ok) << playback.result.message;
    ASSERT_TRUE(playback.movie.has_value());
    EXPECT_EQ(
        playback.movie->mode,
        MovieCheckpointMode::ReadOnlyPlayback);
    EXPECT_TRUE(service.RestoreFileArtifact(playback.artifact).result.ok);
}

TEST(StateService, CompatibilityMismatchFailsBeforeBackendMutation)
{
    TemporaryDirectory temp;
    FakeStateBackend backend;
    StateService service(backend);
    ASSERT_TRUE(service.Boot().result.ok);
    const std::filesystem::path state = temp.path() / "other.sav";
    WriteBytes(state, {1, 2, 3});

    const StateFileArtifactReceipt imported =
        service.ImportFileArtifact({
            .path = state,
            .expected_sha256 = hash::sha256_of_file(state.string()),
            .compatibility = Compatibility('b'),
            .movie_mode = ExternalMovieImportMode::NoMovie,
        });

    EXPECT_FALSE(imported.result.ok);
    EXPECT_EQ(
        imported.result.code,
        StateServiceErrorCode::CompatibilityMismatch);
    EXPECT_EQ(
        std::count(
            backend.calls.begin(),
            backend.calls.end(),
            "restore-file"),
        0);
}

TEST(
    StateServiceOwnership,
    RejectsOffActorOperationsWithoutBackendOrParticipantCalls)
{
    FakeStateBackend backend;
    StateService service(backend);
    RecordingParticipant participant;

    StateServiceResult registered;
    StateOperationReceipt booted;
    StateOperationReceipt rebooted;
    StateServiceResult stopped;
    StateHandleReceipt captured_handle;
    StateOperationReceipt restored_handle;
    StateServiceResult released_handle;
    std::optional<StateHandleReceipt> described_handle;
    StateFileArtifactReceipt captured_file;
    StateFileArtifactReceipt imported_file;
    StateOperationReceipt restored_file;
    StateServiceResult released_file;
    std::optional<StateFileArtifactReceipt> described_file;
    std::thread other([&] {
        registered = service.RegisterParticipant(participant);
        booted = service.Boot();
        rebooted = service.Reboot();
        stopped = service.Shutdown();
        captured_handle = service.CaptureMemoryHandle();
        restored_handle =
            service.RestoreMemoryHandle(StateHandleId(1));
        released_handle =
            service.ReleaseMemoryHandle(StateHandleId(1));
        described_handle =
            service.DescribeMemoryHandle(StateHandleId(1));
        captured_file = service.CaptureFileArtifact(
            {.path = "off-thread.sav"});
        imported_file = service.ImportFileArtifact({});
        restored_file =
            service.RestoreFileArtifact(StateArtifactId(1));
        released_file =
            service.ReleaseFileArtifact(StateArtifactId(1));
        described_file =
            service.DescribeFileArtifact(StateArtifactId(1));
    });
    other.join();

    EXPECT_FALSE(registered.ok);
    EXPECT_EQ(registered.code, StateServiceErrorCode::InvalidState);
    EXPECT_FALSE(booted.result.ok);
    EXPECT_EQ(booted.result.code, StateServiceErrorCode::InvalidState);
    EXPECT_FALSE(rebooted.result.ok);
    EXPECT_FALSE(stopped.ok);
    EXPECT_FALSE(captured_handle.result.ok);
    EXPECT_FALSE(restored_handle.result.ok);
    EXPECT_FALSE(released_handle.ok);
    EXPECT_FALSE(described_handle.has_value());
    EXPECT_FALSE(captured_file.result.ok);
    EXPECT_FALSE(imported_file.result.ok);
    EXPECT_FALSE(restored_file.result.ok);
    EXPECT_FALSE(released_file.ok);
    EXPECT_FALSE(described_file.has_value());
    EXPECT_TRUE(backend.calls.empty());
    EXPECT_TRUE(participant.calls.empty());
    EXPECT_EQ(service.current_epoch(), StateEpoch{});
    EXPECT_FALSE(service.is_open());
}

} // namespace
