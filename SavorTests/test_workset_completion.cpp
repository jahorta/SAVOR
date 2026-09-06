#include <gtest/gtest.h>

#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Runner/Runtime/Worksets/SavestateArtifactFinalizer.h"
#include "Runner/Runtime/Worksets/ProgramBaseline.h"
#include "Runner/Runtime/Worksets/WorkerCompletionLedger.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Utils/Hash.h"
#include "Utils/FilesystemPath.h"
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

class RecordingNotifier final : public ISavestateArtifactFinalizerNotifier
{
public:
    void NotifySavestateArtifactFinalizerCompletion() noexcept override
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
    const auto phase = seedprobe::SeedProbeFullPhaseDefinitionV2();
    definition.phase_invocation = {
        .invocation_id = {7, 9},
        .program_package =
            fullphase::BuildFullPhaseProgramPackage(*phase),
        .common_input = fullphase::MakeFullPhaseCommonInput(
            "soa.seed_probe.CommonInput", 1),
    };
    definition.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = "codec.sav",
        .state_sha256 = std::string(64, '0'),
        .compatibility = {
            "GEAE8E",
            std::string(64, '1'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "codec", .producer = "test"},
    };
    definition.baseline.lineage =
        phase->runtime_contract().baseline_lineage;
    definition.baseline.components.push_back({
        "test.derived-memory",
        1,
        "test.bytes/1",
        hash::sha256("abc", 3),
        ProgramBaselineComponentPolicy::ResetForEveryItem,
        {'a', 'b', 'c'}});
    definition.execution_key.module =
        phase->runtime_contract().module;
    definition.execution_key.entrypoint =
        phase->runtime_contract().entrypoint;
    definition.execution_key.verified_dependency_sha256 =
        phase->runtime_contract().verified_dependency_sha256;
    definition.execution_key.runtime_profile_sha256 =
        phase->runtime_contract().runtime_profile_sha256;
    definition.execution_key.baseline =
        ComputeProgramBaselineKey(definition.baseline);
    definition.execution_key.movie_policy_sha256 =
        phase->runtime_contract().movie_policy_sha256;
    definition.execution_key.service_policy_sha256 =
        phase->runtime_contract().service_policy_sha256;
    definition.execution_key.program_package_sha256 =
        definition.phase_invocation.program_package.canonical_sha256;
    definition.execution_key.common_input_sha256 =
        definition.phase_invocation.common_input.content_sha256;
    definition.execution_key.derived_state_binding_sha256 =
        definition.derived_state.content_sha256;
    definition.execution_key.capture_binding_sha256 =
        EmptyWorksetCaptureBindingHashV1();
    definition.execution_key.progress_plan_sha256 =
        definition.progress_plan.content_sha256;
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);

    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.ordinal = 0;
    savor::GCInputFrame input_frame{};
    input_frame.buttons = 0xB2A1;
    input_frame.main_x = 0xC3;
    input_frame.main_y = 0xD4;
    input_frame.c_x = 0xE5;
    input_frame.c_y = 0xF6;
    input_frame.trig_l = 0x17;
    input_frame.trig_r = 0x28;
    item.execution = {
        ProgramExecutionId(2),
        AttemptId(3),
        seedprobe::EncodeSeedProbeExecutionInputV2(
            {input_frame})};
    item.declared_terminal_bytes = 4096;
    item.correlation = {"job", "claim", "parent"};
    definition.items.push_back(std::move(item));
    return definition;
}

WorkerRuntimeContractV1 CodecRuntimeContract()
{
    return BuildProductionWorkerRuntimeContractV1();
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

TEST(WorksetWireCodec, RoundTripsCompositeBaselineAndRuntimeContract)
{
    WorkerWorksetDefinition definition =
        CodecWorksetDefinition();

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodeWorkerWorksetV5(definition, encoded));
    WorkerWorksetDefinition decoded;
    ASSERT_TRUE(DecodeWorkerWorksetV5(encoded, decoded));
    definition.encoded_size_bytes = encoded.size();
    EXPECT_EQ(decoded, definition);

    const WorkerRuntimeContractV1 contract = CodecRuntimeContract();
    ASSERT_TRUE(EncodeWorkerRuntimeContractV1(contract, encoded));
    WorkerRuntimeContractV1 decoded_contract;
    ASSERT_TRUE(
        DecodeWorkerRuntimeContractV1(
            encoded,
            decoded_contract));
    EXPECT_EQ(decoded_contract, contract);
}

TEST(WorksetWireCodec, RoundTripsStandaloneObservationBindings)
{
    std::vector<std::uint8_t> encoded;
    std::optional<WorksetCaptureBindingV1> capture;
    ASSERT_TRUE(EncodeWorksetCaptureBindingV1(capture, encoded));
    std::optional<WorksetCaptureBindingV1> decoded_capture{
        WorksetCaptureBindingV1{}};
    ASSERT_TRUE(DecodeWorksetCaptureBindingV1(
        encoded,
        decoded_capture));
    EXPECT_FALSE(decoded_capture.has_value());

    constexpr std::array<std::string_view, 1> libraries{
        "soa.progress.battle.events/1",
    };
    const auto progress_plan =
        progress::ResolveProgressPlanV1(libraries);
    ASSERT_TRUE(EncodeProgressPlanV1(progress_plan, encoded));
    progress::ProgressPlanV1 decoded_progress;
    ASSERT_TRUE(DecodeProgressPlanV1(encoded, decoded_progress));
    EXPECT_EQ(decoded_progress, progress_plan);
}

TEST(WorksetWireCodec, RejectsWorksetV3WithoutPublishingOutput)
{
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodeWorkerWorksetV5(
        CodecWorksetDefinition(), encoded));
    ASSERT_GE(encoded.size(), 4u);
    encoded[0] = 3;
    encoded[1] = 0;
    encoded[2] = 0;
    encoded[3] = 0;

    WorkerWorksetDefinition output;
    output.workset_id = WorkerWorksetId(999);
    const WorkerWorksetDefinition unchanged = output;
    EXPECT_FALSE(DecodeWorkerWorksetV5(encoded, output));
    EXPECT_EQ(output, unchanged);
}

TEST(
    WorksetWireCodec,
    RejectsLegacyItemActiveBudgetBeforePublishingOutput)
{
    const WorkerWorksetDefinition definition =
        CodecWorksetDefinition();
    std::vector<std::uint8_t> legacy;
    ASSERT_TRUE(EncodeWorkerWorksetV5(definition, legacy));

    // The former v1 item layout placed declared_active_budget (u64 ms)
    // immediately after the encoded invocation template payload.
    const std::vector<std::uint8_t> payload_marker{
        8, 0, 0, 0,
        0xa1, 0xb2, 0xc3, 0xd4,
        0xe5, 0xf6, 0x17, 0x28};
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
        DecodeWorkerWorksetV5(legacy, output);
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
    const auto phase = seedprobe::SeedProbeFullPhaseDefinitionV2();
    definition.phase_invocation = {
        .invocation_id = {2, 1},
        .program_package =
            fullphase::BuildFullPhaseProgramPackage(*phase),
        .common_input = fullphase::MakeFullPhaseCommonInput(
            "soa.seed_probe.CommonInput", 1),
    };
    definition.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = "validation.sav",
        .state_sha256 = std::string(64, '0'),
        .compatibility = {
            "GEAE8E",
            std::string(64, '1'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "validation", .producer = "test"},
    };
    definition.baseline.lineage =
        phase->runtime_contract().baseline_lineage;
    definition.execution_key.module =
        phase->runtime_contract().module;
    definition.execution_key.entrypoint =
        phase->runtime_contract().entrypoint;
    definition.execution_key.verified_dependency_sha256 =
        phase->runtime_contract().verified_dependency_sha256;
    definition.execution_key.runtime_profile_sha256 =
        phase->runtime_contract().runtime_profile_sha256;
    definition.execution_key.baseline =
        ComputeProgramBaselineKey(definition.baseline);
    definition.execution_key.movie_policy_sha256 =
        phase->runtime_contract().movie_policy_sha256;
    definition.execution_key.service_policy_sha256 =
        phase->runtime_contract().service_policy_sha256;
    definition.execution_key.program_package_sha256 =
        definition.phase_invocation.program_package.canonical_sha256;
    definition.execution_key.common_input_sha256 =
        definition.phase_invocation.common_input.content_sha256;
    definition.execution_key.derived_state_binding_sha256 =
        definition.derived_state.content_sha256;
    definition.execution_key.capture_binding_sha256 =
        EmptyWorksetCaptureBindingHashV1();
    definition.execution_key.progress_plan_sha256 =
        definition.progress_plan.content_sha256;
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);
    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.execution = {
        ProgramExecutionId(4),
        AttemptId(5),
        seedprobe::EncodeSeedProbeExecutionInputV2(
            {savor::GCInputFrame{}})};
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

TEST(WorkerRuntimeContract, HashesCanonicalShapeAndRejectsInvalidLimits)
{
    WorkerRuntimeContractV1 contract =
        BuildProductionWorkerRuntimeContractV1();
    EXPECT_TRUE(ValidateWorkerRuntimeContractV1(contract).ok);

    WorkerRuntimeContractV1 changed = contract;
    changed.build_identity += ".different";
    EXPECT_FALSE(ValidateWorkerRuntimeContractV1(changed).ok);
    changed.canonical_sha256 =
        ComputeWorkerRuntimeContractHashV1(changed);
    EXPECT_TRUE(ValidateWorkerRuntimeContractV1(changed).ok);

    contract.limits.maximum_item_credits = 0;
    contract.canonical_sha256 =
        ComputeWorkerRuntimeContractHashV1(contract);
    EXPECT_FALSE(ValidateWorkerRuntimeContractV1(contract).ok);
}

TEST(FullPhaseWorksetPolicy, EveryTasMovieKindRequiresExactlyOneItem)
{
    const auto validation =
        tasmovie::TasMovieValidationFullPhaseDefinitionV1();
    const auto sterilization = tasmovie::
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1();

    const std::array<const fullphase::IFullPhaseProgramDefinition*, 2>
        phases{validation.get(), sterilization.get()};
    for (const fullphase::IFullPhaseProgramDefinition* phase : phases)
    {
        const fullphase::FullPhaseWorksetPolicy policy =
            phase->workset_policy();
        EXPECT_TRUE(policy.accepts(1));
        EXPECT_FALSE(policy.accepts(0));
        EXPECT_FALSE(policy.accepts(2));
    }
}

TEST(FullPhaseWorksetPolicy, RejectsMultiItemTasMovieWorksetsDuringAdmission)
{
    WorkerWorksetLimits limits;
    const auto validation =
        tasmovie::TasMovieValidationFullPhaseDefinitionV1();
    const auto sterilization = tasmovie::
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1();

    const std::array<const fullphase::IFullPhaseProgramDefinition*, 2>
        phases{validation.get(), sterilization.get()};
    for (const fullphase::IFullPhaseProgramDefinition* phase : phases)
    {
        WorkerWorksetDefinition definition = CodecWorksetDefinition();
        definition.phase_invocation.program_package =
            fullphase::BuildFullPhaseProgramPackage(*phase);
        definition.phase_invocation.common_input =
            fullphase::MakeFullPhaseCommonInput(
                "test.tas_movie.CommonInput",
                1);
        definition.baseline.lineage =
            phase->runtime_contract().baseline_lineage;
        definition.execution_key.module =
            phase->runtime_contract().module;
        definition.execution_key.entrypoint =
            phase->runtime_contract().entrypoint;
        definition.execution_key.verified_dependency_sha256 =
            phase->runtime_contract().verified_dependency_sha256;
        definition.execution_key.runtime_profile_sha256 =
            phase->runtime_contract().runtime_profile_sha256;
        definition.execution_key.baseline =
            ComputeProgramBaselineKey(definition.baseline);
        definition.execution_key.movie_policy_sha256 =
            phase->runtime_contract().movie_policy_sha256;
        definition.execution_key.service_policy_sha256 =
            phase->runtime_contract().service_policy_sha256;
        definition.execution_key.program_package_sha256 =
            definition.phase_invocation.program_package.canonical_sha256;
        definition.execution_key.common_input_sha256 =
            definition.phase_invocation.common_input.content_sha256;
        definition.execution_key.derived_state_binding_sha256 =
            definition.derived_state.content_sha256;
        definition.execution_key.capture_binding_sha256 =
            EmptyWorksetCaptureBindingHashV1();
        definition.execution_key.progress_plan_sha256 =
            definition.progress_plan.content_sha256;
        definition.execution_key.canonical_sha256 =
            ComputeWorkerWorksetExecutionKeyHash(definition.execution_key);

        WorksetItemTemplate second = definition.items.front();
        second.item_id = WorkerWorksetItemId(2);
        second.ordinal = 1;
        second.execution.execution_id = ProgramExecutionId(3);
        second.execution.attempt_id = AttemptId(4);
        definition.items.push_back(std::move(second));

        const WorksetValidationResult result =
            ValidateWorkerWorksetDefinition(definition, limits);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.code, WorkerRejectionCode::InvalidArgument);
        EXPECT_EQ(
            result.error.message,
            "WorkerWorkset item count violates its Full Phase program-kind policy");
    }
}

TEST(
    WorksetStateCoordinator,
    OwnsMultiItemHandleOnlyUntilTheActiveWorksetTerminates)
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
    const ArtifactCompatibilityToken compatibility{
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
        definition.artifact = ProgramBaselineArtifact{
            .kind = ProgramBaselineArtifactKind::Savestate,
            .state_path = path,
            .state_sha256 =
                hash::sha256_of_file(path.string()),
            .compatibility = compatibility,
            .lineage = {
                .edge = "source",
                .producer = "test"},
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
        coordinator.Initialize(
            WorkerWorksetId(1),
            first,
            true,
            receipt)
            .ok);
    EXPECT_TRUE(receipt.state_established);
    ASSERT_TRUE(session.BeginWorksetItemReset(WorkerWorksetId(1)).ok);
    EXPECT_FALSE(session.execution_snapshot());
    ASSERT_TRUE(coordinator.RestoreForNextItem(receipt).ok);
    ASSERT_TRUE(session.execution_snapshot());
    EXPECT_TRUE(receipt.state_established);
    {
        std::lock_guard lock(backend->mutex);
        EXPECT_EQ(backend->restore_buffer_count, 1);
        EXPECT_EQ(backend->restore_file_count, 1);
    }
    ASSERT_TRUE(coordinator.Release().ok);

    ASSERT_TRUE(
        coordinator.Initialize(
            WorkerWorksetId(2),
            first,
            true,
            receipt)
            .ok);
    EXPECT_TRUE(receipt.state_established);
    ASSERT_TRUE(coordinator.Release().ok);
    {
        std::lock_guard lock(backend->mutex);
        EXPECT_EQ(backend->restore_buffer_count, 1);
        EXPECT_EQ(backend->restore_file_count, 2);
    }

    EXPECT_TRUE(coordinator.Shutdown().ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(
    WorksetStateCoordinator,
    RestoreFailurePreservesEpochAndTaintsOnlyForUnknownIntegrity)
{
    TemporaryDirectory temp;
    const std::filesystem::path state_path = temp.path() / "baseline.sav";
    {
        std::ofstream output(
            state_path,
            std::ios::binary | std::ios::trunc);
        output << "baseline-state";
    }
    ProgramBaselineDefinition baseline;
    baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = state_path,
        .state_sha256 = hash::sha256_of_file(state_path.string()),
        .compatibility = {
            .game_id = "TEST00",
            .iso_sha256 = std::string(64, '0'),
            .emulator_build = "scripted-dolphin-backend",
            .runtime_revision = "slice4"},
        .lineage = {
            .edge = "source",
            .producer = "test"},
    };
    baseline.lineage = "restore-failure";

    auto backend = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(91),
        std::make_unique<ScriptedDolphinBackend>(backend));
    SessionOpenOptions options;
    options.backend.iso_path = "fake.iso";
    ASSERT_TRUE(session.Open(options).ok);

    WorksetStateCoordinator coordinator(
        session,
        WorkerWorksetLimits{},
        std::make_shared<ProgramBaselineComponentRegistry>());
    ASSERT_TRUE(coordinator.Stage(baseline).ok);
    PreparedProgramBaselineReceipt receipt;
    ASSERT_TRUE(coordinator.Initialize(
        WorkerWorksetId(10), baseline, true, receipt).ok);
    const WorksetEpoch preserved_epoch = receipt.workset_epoch;
    backend->SetRestoreBufferResult(BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "preserved restore failure",
        BackendIntegrity::Preserved));
    ASSERT_TRUE(session.BeginWorksetItemReset(WorkerWorksetId(10)).ok);
    const ProgramBaselineComponentResult preserved =
        coordinator.RestoreForNextItem(receipt);
    EXPECT_FALSE(preserved.ok);
    EXPECT_EQ(session.snapshot().workset_epoch, preserved_epoch);
    EXPECT_EQ(session.snapshot().disposition, SessionDisposition::Clean);
    backend->SetRestoreBufferResult(BackendResult::Success());
    ASSERT_TRUE(coordinator.Release().ok);

    ASSERT_TRUE(coordinator.Initialize(
        WorkerWorksetId(11), baseline, true, receipt).ok);
    const WorksetEpoch unknown_epoch = receipt.workset_epoch;
    backend->SetRestoreBufferResult(BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "unknown restore failure",
        BackendIntegrity::Unknown));
    ASSERT_TRUE(session.BeginWorksetItemReset(WorkerWorksetId(11)).ok);
    const ProgramBaselineComponentResult unknown =
        coordinator.RestoreForNextItem(receipt);
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(session.snapshot().workset_epoch, unknown_epoch);
    EXPECT_EQ(session.snapshot().disposition, SessionDisposition::Tainted);

    EXPECT_TRUE(coordinator.Release().ok);
    EXPECT_TRUE(coordinator.Shutdown().ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(
    WorksetStateCoordinator,
    CommitUsesPostRestorePausedPcRatherThanPreInitializationState)
{
    TemporaryDirectory temp;
    const std::filesystem::path state_path = temp.path() / "baseline.sav";
    {
        std::ofstream output(
            state_path,
            std::ios::binary | std::ios::trunc);
        output << "baseline-at-prebattle";
    }
    ProgramBaselineDefinition baseline;
    baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = state_path,
        .state_sha256 = hash::sha256_of_file(state_path.string()),
        .compatibility = {
            .game_id = "TEST00",
            .iso_sha256 = std::string(64, '0'),
            .emulator_build = "scripted-dolphin-backend",
            .runtime_revision = "slice4"},
        .lineage = {
            .edge = "tasmovie.validation/sterilized-checkpoint",
            .producer = "test"},
    };
    baseline.lineage = "post-restore-authority";

    auto backend = std::make_shared<ScriptedDolphinBackendControl>();
    backend->open_core_state = BackendCoreState::Running;
    backend->pc = 0x8000A1DCu;
    backend->restore_file_pc = 0x80101E48u;
    backend->restore_file_core_state = BackendCoreState::Paused;
    EmulationSession session(
        SessionId(92),
        std::make_unique<ScriptedDolphinBackend>(backend));
    SessionOpenOptions options;
    options.backend.iso_path = "fake.iso";
    ASSERT_TRUE(session.Open(options).ok);
    EXPECT_FALSE(session.execution_snapshot());

    WorksetStateCoordinator coordinator(
        session,
        WorkerWorksetLimits{},
        std::make_shared<ProgramBaselineComponentRegistry>());
    ASSERT_TRUE(coordinator.Stage(baseline).ok);
    PreparedProgramBaselineReceipt receipt;
    const ProgramBaselineComponentResult initialized = coordinator.Initialize(
        WorkerWorksetId(12), baseline, false, receipt);
    ASSERT_TRUE(initialized.ok) << initialized.error.message;

    const std::optional<ExecutionSnapshot> execution =
        session.execution_snapshot();
    ASSERT_TRUE(execution);
    EXPECT_EQ(execution->evidence.pc, 0x80101E48u);
    EXPECT_EQ(execution->activity, ExecutionActivity::IdlePaused);
    EXPECT_EQ(backend->restore_file_count, 1);

    EXPECT_TRUE(coordinator.Release().ok);
    EXPECT_TRUE(coordinator.Shutdown().ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(SavestateArtifactFinalizer, PublishesImmutableSidecarsThenStateAndDrains)
{
    TemporaryDirectory temp;
    WorkerWorksetLimits limits;
    limits.finalizer_threads = 2;
    limits.maximum_pending_finalizers = 8;
    limits.maximum_pending_finalizer_bytes = 256;
    auto notifier = std::make_shared<RecordingNotifier>();
    SavestateArtifactFinalizer finalizer(limits, notifier);

    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const WorkerTerminalReservation terminal =
        ledger.ReserveTerminal(Correlation(1), 64);
    ASSERT_TRUE(terminal.result.ok);

    const auto state = Bytes("immutable-state");
    const auto movie = Bytes("immutable-movie");
    SavestateArtifactFinalizationRequest request;
    request.terminal = terminal.correlation;
    request.state_artifact_id = SavestateArtifactId(1);
    request.logical_artifact_id = "state:1";
    request.state = {
        temp.path() / "checkpoint.sav",
        state,
        hash::sha256(state.data(), state.size())};
    request.sidecars.push_back({
        temp.path() / "checkpoint.sav.dtm",
        movie,
        hash::sha256(movie.data(), movie.size())});

    const SavestateArtifactFinalizerSubmission submitted =
        finalizer.Submit(std::move(request));
    ASSERT_TRUE(submitted.result.ok) << submitted.result.message;

    SavestateArtifactFinalizationRequest second;
    second.terminal = terminal.correlation;
    second.state_artifact_id = SavestateArtifactId(2);
    second.logical_artifact_id = "state:2";
    second.state = {
        temp.path() / "second.sav",
        Bytes("second-state"),
        {}};
    ASSERT_TRUE(finalizer.Submit(std::move(second)).result.ok);

    SavestateArtifactFinalizationRequest duplicate;
    duplicate.terminal = terminal.correlation;
    duplicate.state_artifact_id = SavestateArtifactId(1);
    duplicate.logical_artifact_id = "state:1-duplicate";
    duplicate.state = {
        temp.path() / "duplicate.sav",
        Bytes("duplicate"),
        {}};
    EXPECT_EQ(
        finalizer.Submit(std::move(duplicate)).result.code,
        SavestateArtifactFinalizerErrorCode::InvalidArgument);

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

    auto completions = finalizer.DrainResults();
    ASSERT_EQ(completions.size(), 2u);
    const auto first = std::ranges::find(
        completions,
        SavestateArtifactId(1),
        &SavestateArtifactFinalizationCompletion::state_artifact_id);
    const auto second_completion = std::ranges::find(
        completions,
        SavestateArtifactId(2),
        &SavestateArtifactFinalizationCompletion::state_artifact_id);
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

TEST(SavestateArtifactFinalizer, RejectsCapacityAndNeverOverwritesConflict)
{
    TemporaryDirectory temp;
    WorkerWorksetLimits limits;
    limits.finalizer_threads = 1;
    limits.maximum_pending_finalizers = 1;
    limits.maximum_pending_finalizer_bytes = 3;
    SavestateArtifactFinalizer finalizer(limits);

    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const WorkerTerminalReservation terminal =
        ledger.ReserveTerminal(Correlation(1), 64);
    ASSERT_TRUE(terminal.result.ok);

    SavestateArtifactFinalizationRequest too_large;
    too_large.terminal = terminal.correlation;
    too_large.state_artifact_id = SavestateArtifactId(1);
    too_large.logical_artifact_id = "state:1";
    too_large.state = {temp.path() / "large.sav", Bytes("1234"), {}};
    EXPECT_EQ(
        finalizer.Submit(std::move(too_large)).result.code,
        SavestateArtifactFinalizerErrorCode::CapacityExceeded);

    {
        std::ofstream existing(
            temp.path() / "conflict.sav",
            std::ios::binary | std::ios::trunc);
        existing << "old";
    }
    SavestateArtifactFinalizationRequest conflict;
    conflict.terminal = terminal.correlation;
    conflict.state_artifact_id = SavestateArtifactId(2);
    conflict.logical_artifact_id = "state:2";
    conflict.state = {temp.path() / "conflict.sav", Bytes("new"), {}};
    ASSERT_TRUE(finalizer.Submit(std::move(conflict)).result.ok);
    finalizer.Shutdown();

    auto completions = finalizer.DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_FALSE(completions[0].result.ok);
    EXPECT_EQ(
        completions[0].result.code,
        SavestateArtifactFinalizerErrorCode::IntegrityFailure);
    EXPECT_EQ(
        hash::sha256_of_file((temp.path() / "conflict.sav").string()),
        hash::sha256("old", 3));
}

TEST(SavestateArtifactFinalizer, SupportsExtendedLengthImmutablePaths)
{
    TemporaryDirectory temp;
    auto long_root = temp.path();
    while ((long_root / "recorded-preseed.sav").native().size() < 300)
        long_root /= "long-finalizer-component-0123456789";

    WorkerWorksetLimits limits;
    limits.finalizer_threads = 1;
    limits.maximum_pending_finalizers = 2;
    limits.maximum_pending_finalizer_bytes = 1024;
    SavestateArtifactFinalizer finalizer(limits);

    WorkerCompletionLedger ledger;
    ASSERT_TRUE(ledger.BindActorThread().ok);
    const auto terminal = ledger.ReserveTerminal(Correlation(1), 64);
    ASSERT_TRUE(terminal.result.ok);

    const auto state = Bytes("long-state");
    const auto movie = Bytes("long-movie");
    SavestateArtifactFinalizationRequest request;
    request.terminal = terminal.correlation;
    request.state_artifact_id = SavestateArtifactId(1);
    request.logical_artifact_id = "state:long";
    request.state = {long_root / "recorded-preseed.sav", state, {}};
    request.sidecars.push_back({long_root / "result.dtm", movie, {}});
    ASSERT_TRUE(finalizer.Submit(std::move(request)).result.ok);
    finalizer.Shutdown();

    const auto completions = finalizer.DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    ASSERT_TRUE(completions.front().result.ok)
        << completions.front().result.message;
    std::filesystem::path native_state;
    ASSERT_TRUE(savor::filesystem::ResolveNativeIoPath(
        long_root / "recorded-preseed.sav", &native_state));
    EXPECT_TRUE(std::filesystem::exists(native_state));
}

} // namespace
