#include "../SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../SavorCore/Runner/Runtime/FullPhase/FullPhaseProgram.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Execution/ProgramExecutor.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "../SavorCore/Tas/DtmFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;
using namespace savor::runtime::tasmovie;

class TemporaryDtm final
{
public:
    TemporaryDtm(std::uint64_t header_count, std::size_t payload_count)
    {
        path_ = std::filesystem::temp_directory_path() /
            ("savor-tas-movie-validation-" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()) +
             ".dtm");
        std::vector<std::uint8_t> bytes(
            0x100 + payload_count * GameCubeDtmInputRecordBytes,
            0);
        bytes[0] = 'D';
        bytes[1] = 'T';
        bytes[2] = 'M';
        bytes[3] = 0x1A;
        bytes[0x00B] = 0x01;
        for (unsigned shift = 0; shift != 64; shift += 8)
        {
            bytes[0x015 + shift / 8] = static_cast<std::uint8_t>(
                header_count >> shift);
        }
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream)
            throw std::runtime_error("failed to create temporary DTM");
    }

    ~TemporaryDtm()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

ProgramModule ProductionModule()
{
    const auto phase = TasMovieValidationFullPhaseDefinitionV1();
    const auto decoded = DecodeProgramModuleV1(
        phase->module_envelope().payload);
    if (!decoded)
        throw std::logic_error(decoded.status.message);
    return std::move(*decoded.value);
}

const Instruction* FindInstruction(
    const ProgramModule& module,
    std::string_view selector)
{
    for (const ProgramFunction& function : module.functions)
    {
        for (const BasicBlock& block : function.blocks)
        {
            const auto found = std::ranges::find(
                block.instructions,
                selector,
                &Instruction::selector);
            if (found != block.instructions.end())
                return &*found;
        }
    }
    return nullptr;
}

TEST(TasMovieValidationContracts, GameCubeRtcPatchCoversFullU32Domain)
{
    TemporaryDtm source(1, 1);
    savor::tas::DtmFile dtm;
    ASSERT_TRUE(dtm.load(source.path()));
    ASSERT_TRUE(dtm.valid());

    dtm.set_gamecube_rtc_seconds(0);
    EXPECT_EQ(dtm.info().recording_start_time, savor::tas::base_sec);
    const auto zero_hash = dtm.compute_sha256();

    dtm.set_gamecube_rtc_seconds(
        std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(
        dtm.info().recording_start_time,
        savor::tas::base_sec
            + std::numeric_limits<std::uint32_t>::max());
    EXPECT_NE(dtm.compute_sha256(), zero_hash);
}

ProgramValueGraph UnitGraph()
{
    ProgramValue value{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::Unit),
        UnitValue{},
    };
    return {value.id, {std::move(value)}};
}

ProgramValueGraph ResourceGraph(
    CanonicalAction action,
    ProgramResourceHandleId handle,
    WorksetEpoch epoch)
{
    const auto contract =
        CanonicalActionResourceContractSchemaIdentity(action);
    if (!contract)
        throw std::logic_error("canonical action is not a resource action");
    ProgramValue value{
        ProgramValueId(1),
        CanonicalActionOutputType(action),
        ResourceHandleValue{handle, *contract, epoch},
    };
    return {value.id, {std::move(value)}};
}

ProgramValueGraph SapReceiptGraph(CanonicalAction action)
{
    ProgramValue value{
        ProgramValueId(1),
        CanonicalActionOutputType(action),
        std::vector<Byte>{'S', 'A', 'P', '1'},
    };
    return {value.id, {std::move(value)}};
}

ProgramValueGraph ContinueObservation(
    ContinueUntilCompletionReasonV1 reason,
    std::uint32_t pc,
    std::uint64_t count,
    std::uint64_t epoch = 5)
{
    std::vector<ProgramValue> values;
    const auto add = [&](TypeRef type, ProgramValuePayload payload)
    {
        const ProgramValueId id(values.size() + 1);
        values.push_back({id, std::move(type), std::move(payload)});
        return id;
    };
    const SchemaIdentity reason_schema = CanonicalRuntimeSchemaIdentity(
        CanonicalRuntimeSchema::ContinueUntilCompletionReason);
    const ProgramValueId reason_id = add(
        TypeRef::Named(reason_schema),
        EnumValue{reason_schema, static_cast<std::int64_t>(reason)});
    const ProgramValueId no_stop = add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt),
        OptionalValue{});
    const ProgramValueId pc_id = add(
        TypeRef::Builtin(BuiltinType::U32),
        pc);
    const ProgramValueId count_id = add(
        TypeRef::Builtin(BuiltinType::U64),
        count);
    const ProgramValueId vi_count_id = add(
        TypeRef::Builtin(BuiltinType::U64),
        0ull);
    const ProgramValueId epoch_id = add(
        TypeRef::Builtin(BuiltinType::U64),
        epoch);
    const ProgramValueId root = add(
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        RecordValue{{
            reason_id,
            no_stop,
            pc_id,
            count_id,
            vi_count_id,
            epoch_id}});
    return {root, std::move(values)};
}

std::shared_ptr<const VerifiedProgramModule> VerifiedProductionModule()
{
    static const auto verified = []
    {
        ProgramModule module = ProductionModule();
        ProgramDefinitionStore modules;
        TypeSchemaRegistry schemas;
        ActionRegistry actions(&schemas);
        CapabilityPackRegistry packs(&schemas, &actions);
        const RegistryResult registered =
            capabilities::RegisterSourceCapabilityPacks(
                schemas,
                actions,
                packs);
        if (!registered.success)
            throw std::logic_error(registered.error.message);
        const auto stored = modules.RegisterCompiled(std::move(module));
        if (!stored.success)
            throw std::logic_error(stored.error.message);
        ProgramVerifier verifier(modules, schemas, actions, packs);
        ProgramVerificationResult result = verifier.Verify(
            stored.module->identity,
            capabilities::SupportedSoaUsaCompatibility());
        if (!result.success || !result.verified)
        {
            throw std::logic_error(
                result.diagnostics.empty()
                    ? "TAS Movie module did not verify"
                    : result.diagnostics.front().message);
        }
        return result.verified;
    }();
    return verified;
}

struct ScriptedModuleResult
{
    ProgramResult terminal;
    std::vector<CanonicalAction> actions;
    std::vector<StagedSavestateOutput> staged_outputs;
};

ScriptedModuleResult RunScriptedModule(
    const TasMovieValidationRequestV1& request,
    std::vector<ProgramValueGraph> continue_results)
{
    const auto phase = TasMovieValidationFullPhaseDefinitionV1();
    std::string diagnostic;
    const auto payload = EncodeTasMovieValidationExecutionInputV1(
        request,
        &diagnostic);
    if (payload.empty())
        throw std::logic_error(diagnostic);
    auto invocation = phase->BuildResolvedExecution(
        payload,
        InvocationId(700),
        AttemptId(701),
        &diagnostic);
    if (!invocation)
        throw std::logic_error(diagnostic);
    invocation->state.expected_session = SessionId(3);
    invocation->state.expected_epoch = WorksetEpoch(5);

    CancellationSource cancellation(invocation->invocation_id);
    ProgramExecutor executor;
    if (!executor.Start(
            VerifiedProductionModule(),
            *invocation,
            cancellation.token(),
            &diagnostic))
    {
        throw std::logic_error(diagnostic);
    }

    std::vector<CanonicalAction> actions;
    std::vector<StagedSavestateOutput> staged_outputs;
    std::size_t continue_index = 0;
    std::uint64_t request_id_value = 1;
    for (int iteration = 0; iteration < 128; ++iteration)
    {
        ProgramExecutorPumpResult pumped = executor.Pump();
        if (pumped.terminal)
        {
            return {
                std::move(*pumped.terminal),
                std::move(actions),
                std::move(staged_outputs),
            };
        }
        if (!pumped.host_request)
            continue;

        const ExecutorHostRequest& host = *pumped.host_request;
        const ProgramActionRequestId request_id(request_id_value++);
        if (!executor.BindPendingAction(request_id))
            throw std::logic_error("could not bind scripted host request");
        ProgramActionResolution completion{
            .request_id = request_id,
            .invocation_id = invocation->invocation_id,
            .attempt_id = invocation->attempt_id,
            .operation = host.operation,
            .status = ProgramActionResolutionStatus::Completed,
            .workset_epoch = executor.snapshot().workset_epoch,
            .output = UnitGraph(),
            .cleanup = ProgramCleanupStatus::Clean,
            .session_disposition = SessionDisposition::Clean,
        };
        if (host.operation == ProgramHostOperation::InvokeAction)
        {
            if (!host.action)
                throw std::logic_error("scripted action has no identity");
            constexpr std::array supported_actions{
                CanonicalAction::MoviePrepareReadOnlyPlayback,
                CanonicalAction::MovieStartPlayback,
                CanonicalAction::ExecutionContinueUntil,
                CanonicalAction::SavestateSaveImmutableArtifact,
            };
            const auto canonical = std::ranges::find_if(
                supported_actions,
                [&](CanonicalAction value)
                {
                    return CanonicalActionIdentity(value) == *host.action;
                });
            if (canonical == supported_actions.end())
            {
                throw std::logic_error("unexpected canonical action");
            }
            actions.push_back(*canonical);
            if (*canonical ==
                    CanonicalAction::MoviePrepareReadOnlyPlayback ||
                *canonical == CanonicalAction::MovieStartPlayback)
            {
                const ProgramResourceHandleId handle(
                    *canonical ==
                            CanonicalAction::MoviePrepareReadOnlyPlayback
                        ? 100
                        : 102);
                completion.output = ResourceGraph(
                    *canonical,
                    handle,
                    completion.workset_epoch);
                completion.resources.push_back({
                    .handle = handle,
                    .receipt = ResourceReceiptId(handle.value()),
                    .kind = *canonical ==
                            CanonicalAction::MoviePrepareReadOnlyPlayback
                        ? ResourceKind::PreparedMoviePlayback
                        : ResourceKind::MovieSession,
                    .acquisition_epoch = completion.workset_epoch,
                });
            }
            else if (*canonical ==
                     CanonicalAction::ExecutionContinueUntil)
            {
                if (continue_index >= continue_results.size())
                    throw std::logic_error("scripted ContinueUntil exhausted");
                completion.output = std::move(
                    continue_results[continue_index++]);
            }
            else
            {
                completion.output = SapReceiptGraph(*canonical);
                staged_outputs.push_back({
                    .artifact_id = "scripted-root-checkpoint",
                    .capture = {
                        .result = SavestateServiceResult::Success(),
                        .artifact = SavestateArtifactId(201),
                        .captured_epoch = completion.workset_epoch,
                        .final_path = "scripted-root-checkpoint.sav",
                        .state_bytes = ImmutableSavestateBytes::Capture(
                            {0x10, 0x20}),
                    },
                });
            }
        }
        if (!executor.DeliverHostCompletion(
                std::move(completion),
                &diagnostic))
        {
            throw std::logic_error(diagnostic);
        }
    }
    throw std::logic_error("scripted TAS Movie module did not terminate");
}

} // namespace

TEST(TasMovieValidationContracts, InputCountUsesCheckedEightByteBoundaries)
{
    EXPECT_EQ(DtmInputCount{0}.PayloadByteOffset(), 0u);
    EXPECT_EQ(DtmInputCount{7}.PayloadByteOffset(), 56u);
    EXPECT_FALSE(DtmInputCount{
        (std::numeric_limits<std::uint64_t>::max)() / 8 + 1}
        .PayloadByteOffset());
}

TEST(TasMovieValidationContracts, Tmi1RoundTripsAndRejectsCorruptionAndInvalidBinding)
{
    constexpr std::uint32_t pc = 0x80101E48u;
    const TasMovieItineraryV1 itinerary{{
        {pc, DtmInputCount{4}},
        {pc, DtmInputCount{9}},
    }};
    std::string diagnostic;
    const auto encoded = EncodeTasMovieItineraryArtifactV1(itinerary, &diagnostic);
    ASSERT_EQ(encoded.size(), 32u) << diagnostic;
    EXPECT_EQ(std::string(encoded.begin(), encoded.begin() + 4), "TMI1");

    TasMovieItineraryV1 decoded;
    ASSERT_TRUE(DecodeTasMovieItineraryArtifactV1(encoded, decoded, &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded, itinerary);
    EXPECT_TRUE(ValidateTasMovieItineraryArtifactV1(decoded, 10, pc, &diagnostic)) << diagnostic;

    auto trailing = encoded;
    trailing.push_back(0);
    EXPECT_FALSE(DecodeTasMovieItineraryArtifactV1(trailing, decoded, &diagnostic));
    auto bad_magic = encoded;
    bad_magic[0] = 'X';
    EXPECT_FALSE(DecodeTasMovieItineraryArtifactV1(bad_magic, decoded, &diagnostic));

    auto unordered = itinerary;
    unordered.checkpoints[1].input_count.value = 4;
    EXPECT_FALSE(ValidateTasMovieItineraryArtifactV1(unordered, 10, pc, &diagnostic));
    EXPECT_FALSE(ValidateTasMovieItineraryArtifactV1(itinerary, 9, pc, &diagnostic));
    EXPECT_FALSE(ValidateTasMovieItineraryArtifactV1(itinerary, 10, 0x80000000u, &diagnostic));

    TasMovieItineraryV1 oversized;
    oversized.checkpoints.resize(MaximumItineraryEntries + 1, {pc, DtmInputCount{0}});
    EXPECT_TRUE(EncodeTasMovieItineraryArtifactV1(oversized, &diagnostic).empty());
}

TEST(TasMovieValidationContracts, MaximumScalarRequestRoundTripsExactly)
{
    TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = std::string(MaximumPathBytes, 'd'),
        .final_checkpoint_path = std::string(MaximumPathBytes, 's'),
    };
    request.itinerary.checkpoints.reserve(MaximumItineraryEntries);
    for (std::uint64_t index = 0;
         index < MaximumItineraryEntries;
         ++index)
    {
        request.itinerary.checkpoints.push_back({
            BeforeRandSeedSetPc,
            DtmInputCount{index},
        });
    }
    std::string diagnostic;
    const std::vector<std::uint8_t> payload =
        EncodeTasMovieValidationExecutionInputV1(
            request,
            &diagnostic);
    ASSERT_FALSE(payload.empty()) << diagnostic;
    TasMovieValidationRequestV1 decoded;
    ASSERT_TRUE(DecodeTasMovieValidationExecutionInputV1(
        payload,
        decoded,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded, request);

    std::vector<std::uint8_t> trailing = payload;
    trailing.push_back(0);
    EXPECT_FALSE(DecodeTasMovieValidationExecutionInputV1(
        trailing,
        decoded,
        &diagnostic));
}

TEST(TasMovieValidationContracts, RejectsIllegalOperationFieldsAndItineraries)
{
    std::string diagnostic;
    TasMovieValidationRequestV1 establishment{
        .operation = TasMovieValidationOperationV1::EstablishRootCursor,
        .dtm_path = "root.dtm",
        .itinerary = {{{BeforeRandSeedSetPc, DtmInputCount{1}}}},
    };
    EXPECT_TRUE(EncodeTasMovieValidationExecutionInputV1(
        establishment,
        &diagnostic).empty());

    TasMovieValidationRequestV1 validation{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = "child.dtm",
    };
    EXPECT_TRUE(EncodeTasMovieValidationExecutionInputV1(
        validation,
        &diagnostic).empty());
    validation.itinerary.checkpoints = {
        {BeforeRandSeedSetPc, DtmInputCount{2}},
        {BeforeRandSeedSetPc, DtmInputCount{2}},
    };
    EXPECT_TRUE(EncodeTasMovieValidationExecutionInputV1(
        validation,
        &diagnostic).empty());
    validation.itinerary.checkpoints = {
        {0x80000000u, DtmInputCount{2}},
    };
    EXPECT_TRUE(EncodeTasMovieValidationExecutionInputV1(
        validation,
        &diagnostic).empty());
}

TEST(TasMovieValidationContracts, ResultCodecEnforcesOutcomeFieldMatrix)
{
    std::string diagnostic;
    const TasMovieValidationResultV1 established{
        .outcome =
            TasMovieValidationOutcomeV1::RootCursorEstablished,
        .candidate_checkpoint = TasMovieCheckpointV1{
            BeforeRandSeedSetPc,
            DtmInputCount{17},
        },
    };
    const TasMovieValidationResultV1 valid{
        .outcome = TasMovieValidationOutcomeV1::Valid,
    };
    const TasMovieValidationResultV1 invalid{
        .outcome = TasMovieValidationOutcomeV1::Invalid,
        .failure = TasMovieValidationFailureV1{
            .reason = TasMovieValidationFailureReasonV1::MovieDesynchronized,
            .diagnostics = {
                .expected_pc = BeforeRandSeedSetPc,
                .expected_input_count = DtmInputCount{17},
                .actual_pc = BeforeRandSeedSetPc,
                .actual_input_count = DtmInputCount{18},
                .last_verified_itinerary_index = 3,
            },
        },
    };
    for (const TasMovieValidationResultV1* source :
         std::array{&established, &valid, &invalid})
    {
        ASSERT_TRUE(ValidateTasMovieValidationResultV1(
            *source,
            &diagnostic)) << diagnostic;
        const ProgramValueGraph graph =
            EncodeTasMovieValidationResultV1(*source);
        ASSERT_TRUE(graph.root);
        TasMovieValidationResultV1 decoded;
        ASSERT_TRUE(DecodeTasMovieValidationResultV1(
            graph,
            decoded,
            &diagnostic)) << diagnostic;
        EXPECT_EQ(decoded, *source);
    }

    TasMovieValidationResultV1 bad_valid = valid;
    bad_valid.candidate_checkpoint = established.candidate_checkpoint;
    EXPECT_FALSE(ValidateTasMovieValidationResultV1(
        bad_valid,
        &diagnostic));
    TasMovieValidationResultV1 bad_invalid = invalid;
    bad_invalid.failure.reset();
    EXPECT_FALSE(ValidateTasMovieValidationResultV1(
        bad_invalid,
        &diagnostic));
}

TEST(TasMovieValidationContracts, Tcs1RoundTripsExactPairAndRejectsDrift)
{
    const TasMovieCheckpointSterilizationRequestV1 request{
        .source_savestate_path = "checkpoint.sav",
        .source_dtm_path = "checkpoint.sav.dtm",
        .output_savestate_path = "sterilized.sav",
    };
    std::string diagnostic;
    const auto encoded =
        EncodeTasMovieCheckpointSterilizationExecutionInputV1(
            request, &diagnostic);
    ASSERT_FALSE(encoded.empty()) << diagnostic;
    TasMovieCheckpointSterilizationRequestV1 decoded;
    ASSERT_TRUE(DecodeTasMovieCheckpointSterilizationExecutionInputV1(
        encoded, decoded, &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded, request);

    auto trailing = encoded;
    trailing.push_back(0);
    EXPECT_FALSE(DecodeTasMovieCheckpointSterilizationExecutionInputV1(
        trailing, decoded, &diagnostic));
    auto mismatched = request;
    mismatched.source_dtm_path = "another.dtm";
    EXPECT_TRUE(EncodeTasMovieCheckpointSterilizationExecutionInputV1(
        mismatched, &diagnostic).empty());
    auto overwriting = request;
    overwriting.output_savestate_path = request.source_savestate_path;
    EXPECT_TRUE(EncodeTasMovieCheckpointSterilizationExecutionInputV1(
        overwriting, &diagnostic).empty());
}

TEST(TasMovieValidationModule, SterilizationDefinitionIsSaveOnlyAndResolvesExactPair)
{
    const auto phase =
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1();
    ASSERT_NE(phase, nullptr);
    EXPECT_EQ(
        phase->identity().program_kind,
        static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize));
    EXPECT_EQ(
        phase->identity().canonical_id,
        SterilizationFullPhaseCanonicalId);
    EXPECT_EQ(phase->runtime_contract().state_policy,
        InvocationStatePolicy::RestoreBaseline);
    EXPECT_FALSE(phase->runtime_contract().execution.allow_movie_playback);
    EXPECT_FALSE(phase->runtime_contract().execution.allow_input);

    const auto decoded_module = DecodeProgramModuleV1(
        phase->module_envelope().payload);
    ASSERT_TRUE(decoded_module) << decoded_module.status.message;
    ASSERT_EQ(decoded_module.value->action_imports.size(), 1u);
    EXPECT_EQ(
        decoded_module.value->action_imports.front().canonical_id,
        CanonicalActionIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact).canonical_id);
    ASSERT_NE(
        FindInstruction(*decoded_module.value,
            "sterilize/save-native-movie-inactive-checkpoint"),
        nullptr);

    const auto root = std::filesystem::temp_directory_path()
        / ("savor-sterilization-contract-"
            + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto source = root / "checkpoint.sav";
    const auto dtm = root / "checkpoint.sav.dtm";
    const auto output = root / "sterilized.sav";
    {
        std::ofstream(source, std::ios::binary).put('s');
        std::ofstream(dtm, std::ios::binary).put('d');
    }
    const TasMovieCheckpointSterilizationRequestV1 request{
        .source_savestate_path = source.string(),
        .source_dtm_path = dtm.string(),
        .output_savestate_path = output.string(),
    };
    std::string diagnostic;
    const auto payload =
        EncodeTasMovieCheckpointSterilizationExecutionInputV1(
            request, &diagnostic);
    const auto invocation = phase->BuildResolvedExecution(
        payload, ProgramExecutionId(901), AttemptId(902), &diagnostic);
    EXPECT_TRUE(invocation.has_value()) << diagnostic;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(TasMovieValidationModule, ProductionDefinitionVerifiesAndIsExactTasMovieKind)
{
    const auto phase = TasMovieValidationFullPhaseDefinitionV1();
    ASSERT_TRUE(phase);
    ASSERT_TRUE(phase->identity());
    EXPECT_EQ(
        phase->identity().program_kind,
        static_cast<std::int32_t>(savor::PK_TasMovie));
    EXPECT_EQ(phase->identity().program_version, 1);
    EXPECT_EQ(phase->identity().contract_revision, 1u);
    EXPECT_EQ(phase->identity().canonical_id, FullPhaseCanonicalId);
    EXPECT_EQ(
        fullphase::ProductionRegistry().Find(
            static_cast<std::int32_t>(savor::PK_TasMovie)),
        phase.get());
    EXPECT_EQ(phase->runtime_contract().state_policy,
              InvocationStatePolicy::EstablishBaseline);
    EXPECT_TRUE(phase->runtime_contract().execution.allow_movie_playback);
    EXPECT_FALSE(phase->runtime_contract().execution.allow_movie_recording);
    EXPECT_FALSE(phase->runtime_contract().execution.allow_input);
    EXPECT_FALSE(phase->runtime_contract().execution.record_trace);

    const auto catalog = TasMovieBoundaryCatalogV1();
    ASSERT_EQ(catalog.size(), 3u);
    for (const auto& [stable_point_id, pc] :
         std::array{
             std::pair{BeforeRandSeedSetPointId, BeforeRandSeedSetPc},
             std::pair{FieldFastPreseedPointId, FieldFastPreseedPc},
             std::pair{
                 FieldDeferredPreseedPointId,
                 FieldDeferredPreseedPc},
         })
    {
        const auto found = std::ranges::find(
            catalog,
            stable_point_id,
            &TasMovieBoundaryCatalogEntryV1::stable_point_id);
        ASSERT_NE(found, catalog.end());
        EXPECT_EQ(found->pc, pc);
    }

    const ProgramModule module = ProductionModule();
    ASSERT_EQ(module.entrypoints.size(), 1u);
    EXPECT_EQ(module.entrypoints.front().name, Entrypoint);
    EXPECT_EQ(module.budgets.maximum_instructions, 2'000'000u);
    EXPECT_EQ(module.budgets.maximum_action_requests, 65'536u);
    EXPECT_EQ(module.budgets.maximum_values, 1'000'000u);
    EXPECT_EQ(module.budgets.maximum_value_bytes,
              256u * 1024u * 1024u);
    EXPECT_EQ(module.budgets.maximum_trace_events, 262'144u);
    EXPECT_EQ(module.action_imports.size(), 4u);
    for (const CanonicalAction expected : {
             CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::SavestateSaveImmutableArtifact,
         })
    {
        EXPECT_NE(
            std::ranges::find(
                module.action_imports,
                CanonicalActionIdentity(expected)),
            module.action_imports.end());
    }
    const Instruction* prepare = FindInstruction(
        module,
        "movie/prepare-and-stop-core");
    const Instruction* playback = FindInstruction(
        module,
        "movie/start-exact-read-only-playback");
    ASSERT_NE(prepare, nullptr);
    ASSERT_NE(playback, nullptr);
    bool ordered = false;
    for (const ProgramFunction& function : module.functions)
    {
        for (const BasicBlock& block : function.blocks)
        {
            const auto prepare_at = std::ranges::find(
                block.instructions,
                prepare->id,
                &Instruction::id);
            const auto playback_at = std::ranges::find(
                block.instructions,
                playback->id,
                &Instruction::id);
            if (prepare_at != block.instructions.end() &&
                playback_at != block.instructions.end())
            {
                ordered = prepare_at < playback_at;
            }
        }
    }
    EXPECT_TRUE(ordered);

    for (std::string_view selector : {
             "root/stop-catalog",
             "validation/stop-catalog",
         })
    {
        const Instruction* config = FindInstruction(module, selector);
        ASSERT_NE(config, nullptr);
        ASSERT_TRUE(config->literal.has_value());
        const auto* bytes = std::get_if<std::vector<Byte>>(
            &config->literal->payload);
        ASSERT_NE(bytes, nullptr);
        ASSERT_GE(bytes->size(), 8u);
        EXPECT_EQ(
            std::string(bytes->begin(), bytes->begin() + 4),
            "SPS1");
        const std::string encoded(bytes->begin(), bytes->end());
        EXPECT_NE(encoded.find(BeforeRandSeedSetPointId),
                  std::string::npos);
        if (selector == "validation/stop-catalog")
        {
            EXPECT_NE(encoded.find(FieldFastPreseedPointId),
                      std::string::npos);
            EXPECT_NE(encoded.find(FieldDeferredPreseedPointId),
                      std::string::npos);
        }
        else
        {
            EXPECT_EQ(encoded.find(FieldFastPreseedPointId),
                      std::string::npos);
            EXPECT_EQ(encoded.find(FieldDeferredPreseedPointId),
                      std::string::npos);
        }
    }
    EXPECT_EQ(FindInstruction(module, "input/acquire"), nullptr);
    EXPECT_EQ(FindInstruction(module, "movie/stop"), nullptr);
    EXPECT_NE(FindInstruction(
        module,
        "capture/save-final-checkpoint-while-paused"), nullptr);
    EXPECT_NE(FindInstruction(
        module,
        "tail/continue-to-movie-end"), nullptr);
}

TEST(TasMovieValidationModule, BuildResolvedExecutionPreflightsDtmAndItinerary)
{
    const auto phase = TasMovieValidationFullPhaseDefinitionV1();
    TemporaryDtm valid_dtm(3, 3);
    TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = valid_dtm.path(),
        .itinerary = {{{
            BeforeRandSeedSetPc,
            DtmInputCount{2},
        }}},
    };
    std::string diagnostic;
    const auto payload = EncodeTasMovieValidationExecutionInputV1(
        request,
        &diagnostic);
    ASSERT_FALSE(payload.empty()) << diagnostic;
    const auto invocation = phase->BuildResolvedExecution(
        payload,
        InvocationId(51),
        AttemptId(52),
        &diagnostic);
    ASSERT_TRUE(invocation) << diagnostic;
    EXPECT_EQ(
        invocation->state.policy,
        InvocationStatePolicy::EstablishBaseline);
    EXPECT_EQ(invocation->entrypoint, Entrypoint);

    request.itinerary.checkpoints.front().input_count = DtmInputCount{3};
    const auto out_of_range = EncodeTasMovieValidationExecutionInputV1(
        request,
        &diagnostic);
    ASSERT_FALSE(out_of_range.empty());
    EXPECT_FALSE(phase->BuildResolvedExecution(
        out_of_range,
        InvocationId(53),
        AttemptId(54),
        &diagnostic));

    TemporaryDtm mismatch(2, 3);
    request.dtm_path = mismatch.path();
    request.itinerary.checkpoints.front().input_count = DtmInputCount{1};
    const auto mismatch_payload =
        EncodeTasMovieValidationExecutionInputV1(
            request,
            &diagnostic);
    ASSERT_FALSE(mismatch_payload.empty());
    EXPECT_FALSE(phase->BuildResolvedExecution(
        mismatch_payload,
        InvocationId(55),
        AttemptId(56),
        &diagnostic));
}

TEST(TasMovieValidationModule, EstablishmentReturnsReachedRootCheckpoint)
{
    TemporaryDtm dtm(3, 3);
    const TasMovieValidationRequestV1 request{
        .operation =
            TasMovieValidationOperationV1::EstablishRootCursor,
        .dtm_path = dtm.path(),
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {ContinueObservation(
            ContinueUntilCompletionReasonV1::Breakpoint,
            BeforeRandSeedSetPc,
            2)});
    EXPECT_EQ(
        run.terminal.infrastructure,
        ProgramInfrastructureStatus::Completed);
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(
        result.outcome,
        TasMovieValidationOutcomeV1::RootCursorEstablished);
    ASSERT_TRUE(result.candidate_checkpoint.has_value());
    EXPECT_EQ(
        *result.candidate_checkpoint,
        (TasMovieCheckpointV1{
            BeforeRandSeedSetPc,
            DtmInputCount{2}}));
    ASSERT_GE(run.actions.size(), 3u);
    EXPECT_EQ(
        run.actions[0],
        CanonicalAction::MoviePrepareReadOnlyPlayback);
    EXPECT_EQ(run.actions[1], CanonicalAction::MovieStartPlayback);
    EXPECT_EQ(run.actions[2], CanonicalAction::ExecutionContinueUntil);
    EXPECT_EQ(
        std::ranges::count(
            run.actions,
            CanonicalAction::SavestateSaveImmutableArtifact),
        0u);
}

TEST(TasMovieValidationModule, EstablishmentMovieEndIsTypedInvalid)
{
    TemporaryDtm dtm(3, 3);
    const TasMovieValidationRequestV1 request{
        .operation =
            TasMovieValidationOperationV1::EstablishRootCursor,
        .dtm_path = dtm.path(),
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {ContinueObservation(
            ContinueUntilCompletionReasonV1::MovieEnded,
            0x80000000u,
            1)});
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.outcome, TasMovieValidationOutcomeV1::Invalid);
    ASSERT_TRUE(result.failure.has_value());
    EXPECT_EQ(
        result.failure->reason,
        TasMovieValidationFailureReasonV1::ExpectedTerminalNotReached);
    EXPECT_FALSE(
        result.failure->diagnostics.expected_input_count.has_value());
    EXPECT_FALSE(
        result.failure->diagnostics
            .last_verified_itinerary_index.has_value());
}

TEST(TasMovieValidationModule, ValidationToleratesEarlierHitsThenRequiresMovieEnd)
{
    TemporaryDtm dtm(5, 5);
    const TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = dtm.path(),
        .itinerary = {{{
            BeforeRandSeedSetPc,
            DtmInputCount{2},
        }}},
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                BeforeRandSeedSetPc,
                1),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                BeforeRandSeedSetPc,
                2),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::MovieEnded,
                0x80000000u,
                5),
        });
    ASSERT_EQ(
        run.terminal.infrastructure,
        ProgramInfrastructureStatus::Completed)
        << (run.terminal.diagnostics.empty()
                ? ""
                : run.terminal.diagnostics.back().message);
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.outcome, TasMovieValidationOutcomeV1::Valid);
    EXPECT_EQ(
        std::ranges::count(
            run.actions,
            CanonicalAction::ExecutionContinueUntil),
        3u);
}

TEST(TasMovieValidationModule,
     ValidationAcceptsDistinctCanonicalFieldBoundaryPoints)
{
    TemporaryDtm dtm(5, 5);
    const TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = dtm.path(),
        .itinerary = {{
            {BeforeRandSeedSetPc, DtmInputCount{1}},
            {FieldFastPreseedPc, DtmInputCount{4}},
        }},
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                BeforeRandSeedSetPc,
                1),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                FieldFastPreseedPc,
                4),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::MovieEnded,
                0x80000000u,
                5),
        });
    ASSERT_EQ(
        run.terminal.infrastructure,
        ProgramInfrastructureStatus::Completed)
        << (run.terminal.diagnostics.empty()
                ? ""
                : run.terminal.diagnostics.back().message);
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.outcome, TasMovieValidationOutcomeV1::Valid);
}

TEST(TasMovieValidationModule, CursorOverrunRetainsLastVerifiedIndex)
{
    TemporaryDtm dtm(5, 5);
    const TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = dtm.path(),
        .itinerary = {{
            {BeforeRandSeedSetPc, DtmInputCount{1}},
            {BeforeRandSeedSetPc, DtmInputCount{3}},
        }},
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                BeforeRandSeedSetPc,
                1),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::CursorOverrun,
                BeforeRandSeedSetPc,
                4),
        });
    ASSERT_EQ(
        run.terminal.infrastructure,
        ProgramInfrastructureStatus::Completed)
        << (run.terminal.diagnostics.empty()
                ? ""
                : run.terminal.diagnostics.back().message);
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.outcome, TasMovieValidationOutcomeV1::Invalid);
    ASSERT_TRUE(result.failure.has_value());
    EXPECT_EQ(
        result.failure->reason,
        TasMovieValidationFailureReasonV1::MovieDesynchronized);
    ASSERT_TRUE(result.failure->diagnostics
                    .last_verified_itinerary_index.has_value());
    EXPECT_EQ(
        *result.failure->diagnostics.last_verified_itinerary_index,
        0u);
}

TEST(TasMovieValidationModule, FinalCheckpointCapturePrecedesTailMovieEnd)
{
    TemporaryDtm dtm(5, 5);
    const TasMovieValidationRequestV1 request{
        .operation = TasMovieValidationOperationV1::Validate,
        .dtm_path = dtm.path(),
        .itinerary = {{{
            BeforeRandSeedSetPc,
            DtmInputCount{2},
        }}},
        .final_checkpoint_path = dtm.path() + ".sav",
    };
    ScriptedModuleResult run = RunScriptedModule(
        request,
        {
            ContinueObservation(
                ContinueUntilCompletionReasonV1::Breakpoint,
                BeforeRandSeedSetPc,
                2),
            ContinueObservation(
                ContinueUntilCompletionReasonV1::MovieEnded,
                0x80000000u,
                5),
        });
    ASSERT_EQ(
        run.terminal.infrastructure,
        ProgramInfrastructureStatus::Completed);
    ASSERT_TRUE(run.terminal.output.has_value());
    TasMovieValidationResultV1 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeTasMovieValidationResultV1(
        *run.terminal.output,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.outcome, TasMovieValidationOutcomeV1::Valid);
    ASSERT_EQ(run.staged_outputs.size(), 1u);
    EXPECT_EQ(
        run.staged_outputs.front().artifact_id,
        "scripted-root-checkpoint");

    const auto first_continue = std::ranges::find(
        run.actions,
        CanonicalAction::ExecutionContinueUntil);
    const auto capture = std::ranges::find(
        run.actions,
        CanonicalAction::SavestateSaveImmutableArtifact);
    const auto tail_continue = capture == run.actions.end()
        ? run.actions.end()
        : std::find(
              std::next(capture),
              run.actions.end(),
              CanonicalAction::ExecutionContinueUntil);
    ASSERT_NE(first_continue, run.actions.end());
    ASSERT_NE(capture, run.actions.end());
    ASSERT_NE(tail_continue, run.actions.end());
    EXPECT_LT(first_continue, capture);
    EXPECT_LT(capture, tail_continue);
}
