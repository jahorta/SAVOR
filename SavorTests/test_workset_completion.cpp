#include <gtest/gtest.h>

#include "Runner/Runtime/Worksets/StateArtifactFinalizer.h"
#include "Runner/Runtime/Worksets/ProgramBaseline.h"
#include "Runner/Runtime/Worksets/WorkerCompletionLedger.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Utils/Hash.h"
#include "common/ScriptedDolphinBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;
using savor::test_support::ScriptedDolphinBackend;
using savor::test_support::ScriptedDolphinBackendControl;

WorkerItemTerminalCorrelation Correlation(std::uint64_t item)
{
    return {
        .workset_id = WorkerWorksetId(11),
        .item_id = WorkerWorksetItemId(item),
        .item_ordinal = static_cast<std::uint32_t>(item - 1),
        .invocation_id = InvocationId(100 + item),
        .attempt_id = AttemptId(200 + item),
    };
}

ImmutableArtifactBytes Bytes(
    std::string value)
{
    return ImmutableArtifactBytes::Capture(
        std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::vector<std::uint8_t> TerminalBytes(std::string value)
{
    return {value.begin(), value.end()};
}

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-workset-completion-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class RecordingNotifier final : public IStateArtifactFinalizerNotifier
{
public:
    void NotifyStateArtifactFinalizerCompletion() noexcept override
    {
        notifications.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<std::uint32_t> notifications{0};
};

void InsertU64LittleEndian(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint64_t value)
{
    std::array<std::uint8_t, sizeof(std::uint64_t)> encoded{};
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        encoded[shift / 8] =
            static_cast<std::uint8_t>(value >> shift);
    }
    bytes.insert(
        bytes.begin() + offset,
        encoded.begin(),
        encoded.end());
}

WorkerWorksetDefinition CodecWorksetDefinition()
{
    WorkerWorksetDefinition definition;
    definition.workset_id = WorkerWorksetId(9);
    definition.baseline.state_kind =
        ProgramBaselineStateKind::CurrentSession;
    definition.baseline.current_session =
        CurrentSessionBaselineGuard{
            SessionId(7),
            StateEpoch(8),
            true};
    definition.baseline.lineage = "lineage";
    definition.baseline.components.push_back({
        "test.derived-memory",
        1,
        "test.bytes/1",
        hash::sha256("abc", 3),
        ProgramBaselineComponentPolicy::ResetForEveryItem,
        {'a', 'b', 'c'}});
    definition.execution_key.module = {
        "test.no_effect/1",
        1,
        std::string(64, 'a')};
    definition.execution_key.entrypoint = "run";
    definition.execution_key.verified_dependency_sha256 =
        std::string(64, 'b');
    definition.execution_key.runtime_profile_sha256 =
        std::string(64, 'c');
    definition.execution_key.baseline =
        ComputeProgramBaselineKey(definition.baseline);
    definition.execution_key.movie_policy_sha256 =
        std::string(64, 'd');
    definition.execution_key.service_policy_sha256 =
        std::string(64, 'e');
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);

    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.ordinal = 0;
    item.invocation = {
        InvocationId(2),
        AttemptId(3),
        definition.execution_key.module,
        definition.execution_key.entrypoint,
        {0xa1, 0xb2, 0xc3}};
    item.declared_terminal_bytes = 4096;
    item.correlation = {"job", "claim", "parent"};
    definition.items.push_back(std::move(item));
    return definition;
}

WorkerRuntimeManifest CodecRuntimeManifest(
    const WorkerWorksetDefinition& definition)
{
    WorkerRuntimeManifest manifest;
    manifest.runtime_profile_sha256 = std::string(64, '1');
    manifest.dependency_manifest_sha256 =
        std::string(64, '2');
    manifest.catalog_status = RuntimeCatalogStatus::Partial;
    manifest.modules.push_back({
        definition.execution_key.module,
        {"run"},
        manifest.dependency_manifest_sha256,
        true});
    manifest.catalog_sha256 = ComputeRuntimeCatalogHash(
        manifest.modules,
        manifest.catalog_status);
    return manifest;
}

TEST(WorkerCompletionLedger, PreservesTerminalOrderAcrossOutOfOrderFinalization)
{
    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const WorkerTerminalReservation first =
        ledger.ReserveTerminal(Correlation(1), 64);
    const WorkerTerminalReservation second =
        ledger.ReserveTerminal(Correlation(2), 64);
    ASSERT_TRUE(first.result.ok) << first.result.message;
    ASSERT_TRUE(second.result.ok) << second.result.message;
    EXPECT_EQ(first.correlation.terminal_order.value(), 1u);
    EXPECT_EQ(second.correlation.terminal_order.value(), 2u);

    ASSERT_TRUE(
        ledger.CompleteTerminal(
            second.correlation,
            TerminalBytes("second")).ok);
    const WorkerTerminalPublicationReceipt blocked =
        ledger.BeginNextPublication();
    ASSERT_TRUE(blocked.result.ok);
    EXPECT_FALSE(blocked.publication);

    ASSERT_TRUE(
        ledger.CompleteTerminal(
            first.correlation,
            TerminalBytes("first")).ok);
    const WorkerTerminalPublicationReceipt first_publication =
        ledger.BeginNextPublication();
    ASSERT_TRUE(first_publication.result.ok);
    ASSERT_TRUE(first_publication.publication);
    EXPECT_EQ(
        first_publication.publication->correlation,
        first.correlation);
    EXPECT_EQ(
        first_publication.publication->outbound_sequence.value(),
        1u);
    ASSERT_TRUE(ledger.ConfirmPublished(first.correlation).ok);

    const WorkerTerminalPublicationReceipt second_publication =
        ledger.BeginNextPublication();
    ASSERT_TRUE(second_publication.result.ok);
    ASSERT_TRUE(second_publication.publication);
    EXPECT_EQ(
        second_publication.publication->correlation,
        second.correlation);
    EXPECT_EQ(
        second_publication.publication->outbound_sequence.value(),
        2u);
    ASSERT_TRUE(ledger.ConfirmPublished(second.correlation).ok);

    WorkerItemTerminalCorrelation mismatched = second.correlation;
    mismatched.attempt_id = AttemptId(999);
    EXPECT_EQ(
        ledger.AcknowledgeTerminal(mismatched).code,
        CompletionLedgerErrorCode::TerminalMismatch);
    EXPECT_TRUE(ledger.AcknowledgeTerminal(second.correlation).ok);
    EXPECT_TRUE(ledger.AcknowledgeTerminal(first.correlation).ok);
    EXPECT_EQ(ledger.snapshot().retained_terminals, 0u);
}

TEST(WorksetWireCodec, RoundTripsCompositeBaselineAndManifest)
{
    WorkerWorksetDefinition definition =
        CodecWorksetDefinition();

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodeWorkerWorksetV1(definition, encoded));
    WorkerWorksetDefinition decoded;
    ASSERT_TRUE(DecodeWorkerWorksetV1(encoded, decoded));
    definition.encoded_size_bytes = encoded.size();
    EXPECT_EQ(decoded, definition);

    WorkerRuntimeManifest manifest =
        CodecRuntimeManifest(definition);
    ASSERT_TRUE(EncodeWorkerRuntimeManifestV1(manifest, encoded));
    WorkerRuntimeManifest decoded_manifest;
    ASSERT_TRUE(
        DecodeWorkerRuntimeManifestV1(
            encoded,
            decoded_manifest));
    EXPECT_EQ(decoded_manifest, manifest);
}

TEST(
    WorksetWireCodec,
    RejectsLegacyItemActiveBudgetBeforePublishingOutput)
{
    const WorkerWorksetDefinition definition =
        CodecWorksetDefinition();
    std::vector<std::uint8_t> legacy;
    ASSERT_TRUE(EncodeWorkerWorksetV1(definition, legacy));

    // The former v1 item layout placed declared_active_budget (u64 ms)
    // immediately after the encoded invocation template payload.
    const std::vector<std::uint8_t> payload_marker{
        3, 0, 0, 0, 0xa1, 0xb2, 0xc3};
    const auto payload = std::search(
        legacy.begin(),
        legacy.end(),
        payload_marker.begin(),
        payload_marker.end());
    ASSERT_NE(payload, legacy.end());
    ASSERT_EQ(
        std::search(
            std::next(payload),
            legacy.end(),
            payload_marker.begin(),
            payload_marker.end()),
        legacy.end());
    InsertU64LittleEndian(
        legacy,
        static_cast<std::size_t>(
            std::distance(legacy.begin(), payload)) +
            payload_marker.size(),
        2000);

    WorkerWorksetDefinition output;
    output.workset_id = WorkerWorksetId(999);
    const WorkerWorksetDefinition unchanged = output;
    const WorksetWireCodecResult rejected =
        DecodeWorkerWorksetV1(legacy, output);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(output, unchanged);
}

TEST(
    WorksetWireCodec,
    RejectsLegacyAggregateBudgetManifestBeforePublishingOutput)
{
    const WorkerWorksetDefinition definition =
        CodecWorksetDefinition();
    const WorkerRuntimeManifest manifest =
        CodecRuntimeManifest(definition);
    std::vector<std::uint8_t> legacy;
    ASSERT_TRUE(
        EncodeWorkerRuntimeManifestV1(manifest, legacy));

    // The limits record is the final fixed-width 64 bytes in the current
    // manifest. Former v1 inserted maximum_aggregate_active_budget (u64 ms)
    // after its first u32/u64 pair.
    constexpr std::size_t kCurrentLimitsSize = 64;
    constexpr std::size_t kLegacyBudgetOffsetInLimits =
        sizeof(std::uint32_t) + sizeof(std::uint64_t);
    ASSERT_GE(legacy.size(), kCurrentLimitsSize);
    InsertU64LittleEndian(
        legacy,
        legacy.size() - kCurrentLimitsSize +
            kLegacyBudgetOffsetInLimits,
        4ull * 60ull * 60ull * 1000ull);

    WorkerRuntimeManifest output;
    output.runtime_profile_sha256 = "unchanged";
    const WorkerRuntimeManifest unchanged = output;
    const WorksetWireCodecResult rejected =
        DecodeWorkerRuntimeManifestV1(legacy, output);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(output, unchanged);
}

TEST(WorkerCompletionLedger, EnforcesCountBytesAndPublicationRetryIdentity)
{
    WorkerWorksetLimits limits;
    limits.maximum_retained_terminals = 1;
    limits.maximum_retained_terminal_bytes = 4;
    WorkerCompletionLedger ledger(limits);
    ASSERT_TRUE(ledger.BindActorThread().ok);

    const WorkerTerminalReservation reserved =
        ledger.ReserveTerminal(Correlation(1), 4);
    ASSERT_TRUE(reserved.result.ok);
    EXPECT_EQ(
        ledger.ReserveTerminal(Correlation(2), 1).result.code,
        CompletionLedgerErrorCode::CapacityExceeded);
    EXPECT_EQ(
        ledger.CompleteTerminal(
            reserved.correlation,
            TerminalBytes("12345")).code,
        CompletionLedgerErrorCode::CapacityExceeded);
    ASSERT_TRUE(
        ledger.CompleteTerminal(
            reserved.correlation,
            TerminalBytes("1234")).ok);

    const auto first = ledger.BeginNextPublication();
    ASSERT_TRUE(first.publication);
    ASSERT_TRUE(ledger.AbortPublication(reserved.correlation).ok);
    EXPECT_EQ(
        ledger.ReserveOutboundSequence().result.code,
        CompletionLedgerErrorCode::InvalidState);
    EXPECT_EQ(
        ledger.snapshot().open_outbound_sequence,
        first.publication->outbound_sequence);
    const auto retried = ledger.BeginNextPublication();
    ASSERT_TRUE(retried.publication);
    EXPECT_EQ(
        retried.publication->outbound_sequence,
        first.publication->outbound_sequence);
    ASSERT_TRUE(ledger.ConfirmPublished(reserved.correlation).ok);
    ASSERT_TRUE(ledger.AcknowledgeTerminal(reserved.correlation).ok);
    EXPECT_TRUE(ledger.AcknowledgeTerminal(reserved.correlation).ok);
    WorkerItemTerminalCorrelation mismatched =
        reserved.correlation;
    mismatched.attempt_id = AttemptId(999);
    EXPECT_EQ(
        ledger.AcknowledgeTerminal(mismatched).code,
        CompletionLedgerErrorCode::TerminalMismatch);
}

TEST(WorksetValidation, CapsEachTerminalBelowTheWrmsPayloadCeiling)
{
    WorkerWorksetLimits limits;
    WorkerWorksetDefinition definition;
    definition.workset_id = WorkerWorksetId(1);
    definition.baseline.state_kind =
        ProgramBaselineStateKind::CurrentSession;
    definition.baseline.current_session =
        CurrentSessionBaselineGuard{
            SessionId(2),
            StateEpoch(3),
            true};
    definition.baseline.lineage = "lineage";
    definition.execution_key.module = {
        "test.module/1",
        1,
        std::string(64, 'a')};
    definition.execution_key.entrypoint = "run";
    definition.execution_key.verified_dependency_sha256 =
        std::string(64, 'b');
    definition.execution_key.runtime_profile_sha256 =
        std::string(64, 'c');
    definition.execution_key.baseline =
        ComputeProgramBaselineKey(definition.baseline);
    definition.execution_key.movie_policy_sha256 =
        std::string(64, 'd');
    definition.execution_key.service_policy_sha256 =
        std::string(64, 'e');
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);
    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.invocation = {
        InvocationId(4),
        AttemptId(5),
        definition.execution_key.module,
        "run",
        {1}};
    item.declared_terminal_bytes =
        kMaximumWorksetTerminalReservationBytes;
    definition.items.push_back(item);
    EXPECT_TRUE(
        ValidateWorkerWorksetDefinition(definition, limits).ok);

    definition.items.front().declared_terminal_bytes =
        kMaximumWorksetTerminalReservationBytes + 1;
    EXPECT_EQ(
        ValidateWorkerWorksetDefinition(definition, limits)
            .error.code,
        WorkerRejectionCode::CapacityExceeded);
}

TEST(WorkerRuntimeManifest, HashesCanonicalShapeAndRejectsInvalidLimits)
{
    WorkerRuntimeManifest manifest;
    manifest.runtime_profile_sha256 = std::string(64, '1');
    manifest.dependency_manifest_sha256 = std::string(64, '2');
    manifest.modules = {{
        {"test.module/1", 1, std::string(64, '3')},
        {"z", "a"},
        manifest.dependency_manifest_sha256,
        true}};
    manifest.catalog_sha256 = ComputeRuntimeCatalogHash(
        manifest.modules,
        manifest.catalog_status);
    EXPECT_TRUE(ValidateWorkerRuntimeManifest(manifest).ok);

    WorkerRuntimeManifest reordered = manifest;
    std::reverse(
        reordered.modules.front().entrypoints.begin(),
        reordered.modules.front().entrypoints.end());
    EXPECT_EQ(
        ComputeRuntimeCatalogHash(
            reordered.modules,
            reordered.catalog_status),
        manifest.catalog_sha256);

    reordered.modules.front().entrypoints.push_back("a");
    reordered.catalog_sha256 = ComputeRuntimeCatalogHash(
        reordered.modules,
        reordered.catalog_status);
    EXPECT_FALSE(ValidateWorkerRuntimeManifest(reordered).ok);

    manifest.limits.maximum_item_credits = 0;
    EXPECT_FALSE(ValidateWorkerRuntimeManifest(manifest).ok);
}

TEST(
    WorksetStateCoordinator,
    ReusesExactCacheFallsBackAfterPreservedFailureAndEvictsLru)
{
    TemporaryDirectory temp;
    const auto write_state =
        [&](std::string name, std::string bytes)
    {
        const std::filesystem::path path =
            temp.path() / std::move(name);
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output.write(
            bytes.data(),
            static_cast<std::streamsize>(bytes.size()));
        output.close();
        return path;
    };
    const std::filesystem::path first_path =
        write_state("first.sav", "first-state");
    const std::filesystem::path second_path =
        write_state("second.sav", "second-state");
    const StateCompatibilityToken compatibility{
        .game_id = "TEST00",
        .iso_sha256 = std::string(64, '0'),
        .emulator_build = "scripted-dolphin-backend",
        .runtime_revision = "slice4",
    };
    const auto baseline =
        [&](const std::filesystem::path& path,
            std::string lineage)
    {
        ProgramBaselineDefinition definition;
        definition.state_kind =
            ProgramBaselineStateKind::Artifact;
        definition.artifact = ProgramBaselineArtifact{
            .state_path = path,
            .state_sha256 =
                hash::sha256_of_file(path.string()),
            .movie_mode =
                ExternalMovieImportMode::NoMovie,
            .compatibility = compatibility,
        };
        definition.lineage = std::move(lineage);
        return definition;
    };

    auto backend =
        std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(90),
        std::make_unique<ScriptedDolphinBackend>(
            backend));
    SessionOpenOptions options;
    options.backend.iso_path = "fake.iso";
    ASSERT_TRUE(session.Open(options).ok);

    WorkerWorksetLimits limits;
    limits.maximum_state_cache_entries = 1;
    limits.maximum_state_cache_bytes = 1024;
    auto components =
        std::make_shared<ProgramBaselineComponentRegistry>();
    WorksetStateCoordinator coordinator(
        session,
        limits,
        components);

    ProgramBaselineDefinition first =
        baseline(first_path, "first");
    ASSERT_TRUE(coordinator.Stage(first).ok);
    PreparedProgramBaselineReceipt receipt;
    ASSERT_TRUE(
        coordinator.Prepare(
            WorkerWorksetId(1),
            first,
            true,
            receipt)
            .ok);
    ASSERT_TRUE(coordinator.Release().ok);

    ASSERT_TRUE(
        coordinator.Prepare(
            WorkerWorksetId(2),
            first,
            true,
            receipt)
            .ok);
    EXPECT_TRUE(receipt.restored);
    ASSERT_TRUE(coordinator.Release().ok);
    {
        std::lock_guard lock(backend->mutex);
        EXPECT_EQ(backend->restore_buffer_count, 1);
        backend->restore_buffer_result =
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "preserved cache restore failure");
    }

    ASSERT_TRUE(
        coordinator.Prepare(
            WorkerWorksetId(3),
            first,
            true,
            receipt)
            .ok);
    EXPECT_FALSE(receipt.restored);
    ASSERT_TRUE(coordinator.Release().ok);
    {
        std::lock_guard lock(backend->mutex);
        EXPECT_EQ(backend->restore_buffer_count, 2);
        EXPECT_GE(backend->restore_file_count, 2);
        backend->restore_buffer_result =
            BackendResult::Success();
    }

    ProgramBaselineDefinition second =
        baseline(second_path, "second");
    ASSERT_TRUE(coordinator.Stage(second).ok);
    ASSERT_TRUE(
        coordinator.Prepare(
            WorkerWorksetId(4),
            second,
            true,
            receipt)
            .ok);
    ASSERT_TRUE(coordinator.Release().ok);
    EXPECT_EQ(coordinator.cache_snapshot().entry_count, 1u);

    const int before_first_again = [&]
    {
        std::lock_guard lock(backend->mutex);
        return backend->restore_file_count;
    }();
    ASSERT_TRUE(
        coordinator.Prepare(
            WorkerWorksetId(5),
            first,
            true,
            receipt)
            .ok);
    EXPECT_FALSE(receipt.restored);
    ASSERT_TRUE(coordinator.Release().ok);
    {
        std::lock_guard lock(backend->mutex);
        EXPECT_GT(
            backend->restore_file_count,
            before_first_again);
    }

    EXPECT_TRUE(coordinator.Shutdown().ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(StateArtifactFinalizer, PublishesImmutableSidecarsThenStateAndDrains)
{
    TemporaryDirectory temp;
    WorkerWorksetLimits limits;
    limits.finalizer_threads = 2;
    limits.maximum_pending_finalizers = 8;
    limits.maximum_pending_finalizer_bytes = 256;
    auto notifier = std::make_shared<RecordingNotifier>();
    StateArtifactFinalizer finalizer(limits, notifier);

    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const WorkerTerminalReservation terminal =
        ledger.ReserveTerminal(Correlation(1), 64);
    ASSERT_TRUE(terminal.result.ok);

    const auto state = Bytes("immutable-state");
    const auto movie = Bytes("immutable-movie");
    StateArtifactFinalizationRequest request;
    request.terminal = terminal.correlation;
    request.state_artifact_id = StateArtifactId(1);
    request.logical_artifact_id = "state:1";
    request.state = {
        temp.path() / "checkpoint.sav",
        state,
        hash::sha256(state.data(), state.size())};
    request.sidecars.push_back({
        temp.path() / "checkpoint.sav.dtm",
        movie,
        hash::sha256(movie.data(), movie.size())});

    const StateArtifactFinalizerSubmission submitted =
        finalizer.Submit(std::move(request));
    ASSERT_TRUE(submitted.result.ok) << submitted.result.message;

    StateArtifactFinalizationRequest second;
    second.terminal = terminal.correlation;
    second.state_artifact_id = StateArtifactId(2);
    second.logical_artifact_id = "state:2";
    second.state = {
        temp.path() / "second.sav",
        Bytes("second-state"),
        {}};
    ASSERT_TRUE(finalizer.Submit(std::move(second)).result.ok);

    StateArtifactFinalizationRequest duplicate;
    duplicate.terminal = terminal.correlation;
    duplicate.state_artifact_id = StateArtifactId(1);
    duplicate.logical_artifact_id = "state:1-duplicate";
    duplicate.state = {
        temp.path() / "duplicate.sav",
        Bytes("duplicate"),
        {}};
    EXPECT_EQ(
        finalizer.Submit(std::move(duplicate)).result.code,
        StateArtifactFinalizerErrorCode::InvalidArgument);

    std::barrier shutdown_start(3);
    std::thread first_shutdown([&] {
        shutdown_start.arrive_and_wait();
        finalizer.Shutdown();
    });
    std::thread second_shutdown([&] {
        shutdown_start.arrive_and_wait();
        finalizer.Shutdown();
    });
    shutdown_start.arrive_and_wait();
    first_shutdown.join();
    second_shutdown.join();

    auto completions = finalizer.DrainCompletions();
    ASSERT_EQ(completions.size(), 2u);
    const auto first = std::ranges::find(
        completions,
        StateArtifactId(1),
        &StateArtifactFinalizationCompletion::state_artifact_id);
    const auto second_completion = std::ranges::find(
        completions,
        StateArtifactId(2),
        &StateArtifactFinalizationCompletion::state_artifact_id);
    ASSERT_NE(first, completions.end());
    ASSERT_NE(second_completion, completions.end());
    ASSERT_TRUE(first->result.ok)
        << first->result.message;
    ASSERT_TRUE(second_completion->result.ok)
        << second_completion->result.message;
    EXPECT_EQ(first->terminal, terminal.correlation);
    EXPECT_EQ(first->state.sha256,
              hash::sha256(state.data(), state.size()));
    ASSERT_EQ(first->sidecars.size(), 1u);
    EXPECT_EQ(first->sidecars[0].sha256,
              hash::sha256(movie.data(), movie.size()));
    EXPECT_TRUE(std::filesystem::exists(temp.path() / "checkpoint.sav"));
    EXPECT_TRUE(
        std::filesystem::exists(temp.path() / "checkpoint.sav.dtm"));
    EXPECT_EQ(notifier->notifications.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(finalizer.snapshot().outstanding_jobs, 0u);
}

TEST(StateArtifactFinalizer, RejectsCapacityAndNeverOverwritesConflict)
{
    TemporaryDirectory temp;
    WorkerWorksetLimits limits;
    limits.finalizer_threads = 1;
    limits.maximum_pending_finalizers = 1;
    limits.maximum_pending_finalizer_bytes = 3;
    StateArtifactFinalizer finalizer(limits);

    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const WorkerTerminalReservation terminal =
        ledger.ReserveTerminal(Correlation(1), 64);
    ASSERT_TRUE(terminal.result.ok);

    StateArtifactFinalizationRequest too_large;
    too_large.terminal = terminal.correlation;
    too_large.state_artifact_id = StateArtifactId(1);
    too_large.logical_artifact_id = "state:1";
    too_large.state = {temp.path() / "large.sav", Bytes("1234"), {}};
    EXPECT_EQ(
        finalizer.Submit(std::move(too_large)).result.code,
        StateArtifactFinalizerErrorCode::CapacityExceeded);

    {
        std::ofstream existing(
            temp.path() / "conflict.sav",
            std::ios::binary | std::ios::trunc);
        existing << "old";
    }
    StateArtifactFinalizationRequest conflict;
    conflict.terminal = terminal.correlation;
    conflict.state_artifact_id = StateArtifactId(2);
    conflict.logical_artifact_id = "state:2";
    conflict.state = {temp.path() / "conflict.sav", Bytes("new"), {}};
    ASSERT_TRUE(finalizer.Submit(std::move(conflict)).result.ok);
    finalizer.Shutdown();

    auto completions = finalizer.DrainCompletions();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_FALSE(completions[0].result.ok);
    EXPECT_EQ(
        completions[0].result.code,
        StateArtifactFinalizerErrorCode::IntegrityFailure);
    EXPECT_EQ(
        hash::sha256_of_file((temp.path() / "conflict.sav").string()),
        hash::sha256("old", 3));
}

} // namespace
