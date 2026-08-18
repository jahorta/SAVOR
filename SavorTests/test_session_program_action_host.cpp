#include <gtest/gtest.h>

#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Runner/Runtime/EmulationSession.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Actions/SessionProgramActionHost.h"
#include "Runner/Runtime/Worksets/SavestateArtifactFinalizer.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Utils/Hash.h"
#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;
using namespace savor::test_support;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        path_ = std::filesystem::temp_directory_path() /
            ("savor-program-action-host-" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path File(
        std::string_view name) const
    {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

class HostHarness final
{
public:
    explicit HostHarness(std::uint64_t session_id = 91)
        : control(
              std::make_shared<
                  ScriptedDolphinBackendControl>()),
          physical_control(
              std::make_shared<
                  FakePhysicalStopBackendControl>()),
          session(
              SessionId(session_id),
              MakeScriptedDolphinBackend(
                  control,
                  MakeFakePhysicalStopBackend(
                      physical_control)))
    {
    }

    bool Open(
        SessionProgramActionHostConfig config = {},
        std::optional<std::filesystem::path> movie = std::nullopt,
        std::optional<MovieCheckpointMetadata> restored_movie = std::nullopt)
    {
        const SessionOperationReceipt opened =
            session.Open({});
        if (!opened.ok)
            return false;
        const SessionOperationReceipt initialization =
            session.OpenWorksetInitialization(WorkerWorksetId(1));
        if (!initialization.ok || session.execution_snapshot())
            return false;
        if (movie)
        {
            MovieService* movies = session.movie_service();
            if (!movies)
                return false;
            const MovieOperationReceipt prepared =
                movies->PrepareReadOnlyPlayback({.dtm_path = *movie});
            if (!prepared.result.ok || !prepared.preparation)
                return false;
        }
        if (restored_movie)
        {
            MovieService* movies = session.movie_service();
            if (!movies)
                return false;
            const SavestateMovieRestoreContext restore{
                initialization.workset_epoch,
                std::move(restored_movie),
                false};
            if (!movies->PrepareSavestateRestore(restore).ok ||
                !movies->CommitSavestateRestore(restore).ok)
            {
                return false;
            }
        }
        const SessionOperationReceipt committed =
            session.CommitWorksetInitialization(WorkerWorksetId(1));
        if (!committed.ok || !session.execution_snapshot())
            return false;
        host = std::make_unique<
            SessionProgramActionHost>(
                session,
                std::move(config));
        return true;
    }

    ProgramActionRequest Request(
        ProgramHostOperation operation,
        WorksetEpoch epoch,
        ProgramScopeId scope = ProgramScopeId(1))
    {
        ProgramActionRequest request;
        request.request_id =
            ProgramActionRequestId(next_request++);
        request.invocation_id = InvocationId(100);
        request.attempt_id = AttemptId(1);
        request.operation = operation;
        request.expected_epoch = epoch;
        request.scope = scope;
        return request;
    }

    ProgramActionDispatchResult Prepare(
        InvocationStatePolicy policy,
        std::string lineage,
        bool state_already_prepared = true)
    {
        const SessionSnapshot snapshot = session.snapshot();
        ProgramActionRequest request = Request(
            ProgramHostOperation::PrepareInvocationState,
            snapshot.workset_epoch);
        request.state_request = InvocationStateRequest{
            .policy = policy,
            .session_lineage = std::move(lineage),
            .expected_session = snapshot.session_id,
            .expected_epoch = snapshot.workset_epoch,
        };
        request.state_already_prepared = state_already_prepared;
        if (state_already_prepared)
            request.prepared_baseline_sha256 = std::string(64, 'b');
        return host->Dispatch(std::move(request));
    }

    ProgramActionDispatchResult Finish()
    {
        return host->Dispatch(Request(
            ProgramHostOperation::FinishInvocation,
            session.snapshot().workset_epoch));
    }

    ProgramActionDispatchResult Invoke(
        CanonicalAction action,
        CanonicalActionPayload payload,
        ProgramScopeId scope = ProgramScopeId(1))
    {
        ProgramActionRequest request = Request(
            ProgramHostOperation::InvokeAction,
            session.snapshot().workset_epoch,
            scope);
        request.action = CanonicalActionIdentity(action);
        const auto schema =
            CanonicalActionInputSchemaIdentity(action);
        if (!schema)
            throw std::logic_error("test action has no input schema");
        CanonicalActionPayloadResult encoded =
            EncodeCanonicalActionPayload(payload, *schema);
        if (!encoded.ok)
            throw std::logic_error(encoded.diagnostic);
        request.input = std::move(encoded.graph);
        return host->Dispatch(std::move(request));
    }

    ProgramActionDispatchResult InvokeGraph(
        CanonicalAction action,
        ProgramValueGraph input,
        ProgramScopeId scope = ProgramScopeId(1))
    {
        ProgramActionRequest request = Request(
            ProgramHostOperation::InvokeAction,
            session.snapshot().workset_epoch,
            scope);
        request.action = CanonicalActionIdentity(action);
        request.input = std::move(input);
        return host->Dispatch(std::move(request));
    }

    std::shared_ptr<ScriptedDolphinBackendControl> control;
    std::shared_ptr<FakePhysicalStopBackendControl>
        physical_control;
    EmulationSession session;
    std::unique_ptr<SessionProgramActionHost> host;
    std::uint64_t next_request = 1;
};

const ProgramValue* Root(const ProgramValueGraph& graph)
{
    const auto found = std::ranges::find(
        graph.values,
        graph.root,
        &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

ProgramActionDispatchResult PrepareMoviePlayback(
    HostHarness& harness,
    const std::filesystem::path& dtm)
{
    CanonicalActionPayload prepare;
    if (!prepare.AddUtf8(
            CanonicalActionPayloadField::Path,
            dtm.string()))
    {
        throw std::logic_error("failed encoding movie preparation path");
    }
    return harness.Invoke(
        CanonicalAction::MoviePrepareReadOnlyPlayback,
        std::move(prepare));
}

ProgramActionDispatchResult StartPreparedMoviePlayback(
    HostHarness& harness,
    const ProgramActionDispatchResult& prepared)
{
    if (!prepared.immediate_result)
        throw std::logic_error("movie preparation did not complete immediately");
    return harness.InvokeGraph(
        CanonicalAction::MovieStartPlayback,
        prepared.immediate_result->resolution.output);
}

class TestValueGraphBuilder final
{
public:
    ProgramValueId Add(
        TypeRef type,
        ProgramValuePayload payload)
    {
        ProgramValue value{
            ProgramValueId(next_id_++),
            std::move(type),
            std::move(payload)};
        const ProgramValueId id = value.id;
        values_.push_back(std::move(value));
        return id;
    }

    ProgramValueId AddFrom(const ProgramValue& source)
    {
        return Add(source.type, source.payload);
    }

    ProgramValueGraph Finish(
        CanonicalAction action,
        std::vector<ProgramValueId> fields)
    {
        const ProgramValueId root = Add(
            CanonicalActionInputType(action),
            RecordValue{std::move(fields)});
        return {root, std::move(values_)};
    }

private:
    std::uint64_t next_id_ = 1;
    std::vector<ProgramValue> values_;
};

class TestStaticConfigWriter final
{
public:
    explicit TestStaticConfigWriter(std::array<char, 4> magic)
    {
        for (char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    }
    void String(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& hash)
    {
        bytes_.insert(
            bytes_.end(),
            hash.bytes.begin(),
            hash.bytes.end());
    }
    std::vector<Byte> Finish() { return std::move(bytes_); }

private:
    std::vector<Byte> bytes_;
};

ProgramValueGraph StartMovieRecordingRequestGraph(
    const ProgramValue& playback,
    const std::filesystem::path& output)
{
    TestStaticConfigWriter writer({'M', 'R', 'C', '1'});
    writer.String(output.string());
    writer.String("test movie recording");
    TestValueGraphBuilder builder;
    const ProgramValueId playback_id = builder.AddFrom(playback);
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::MovieRecordingStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::MovieStartRecording,
        {playback_id, config});
}

ProgramValueGraph AdoptRestoredMoviePlaybackRequestGraph()
{
    TestValueGraphBuilder builder;
    return builder.Finish(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        {});
}

ProgramValueGraph ObserveMovieStateRequestGraph()
{
    TestValueGraphBuilder builder;
    return builder.Finish(CanonicalAction::MovieObserveState, {});
}

ProgramValueGraph StepFramesRequestGraph(
    std::uint64_t count,
    std::vector<Byte> config = {},
    const ProgramValue* input_binding = nullptr)
{
    if (config.empty())
    {
        TestStaticConfigWriter writer({'E', 'A', 'C', '1'});
        writer.U8(2);
        writer.Bool(false);
        writer.U8(0);
        writer.U8(0);
        config = writer.Finish();
    }
    TestValueGraphBuilder builder;
    const ProgramValueId count_id = builder.Add(
        TypeRef::Builtin(BuiltinType::U64),
        count);
    std::optional<ProgramValueId> binding_value;
    if (input_binding)
        binding_value = builder.AddFrom(*input_binding);
    const ProgramValueId binding = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalInputExecutionBinding),
        OptionalValue{binding_value});
    const ProgramValueId static_config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                ExecutionAdvanceStaticConfig),
        std::move(config));
    return builder.Finish(
        CanonicalAction::ExecutionStepFrames,
        {count_id, binding, static_config});
}

ProgramValueGraph RequirePausedPcRequestGraph(
    std::uint32_t expected_pc)
{
    TestValueGraphBuilder builder;
    const ProgramValueId pc = builder.Add(
        TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(expected_pc));
    return builder.Finish(
        CanonicalAction::ExecutionRequirePausedPc,
        {pc});
}

ProgramValueGraph ObservePausedPcRequestGraph()
{
    TestValueGraphBuilder builder;
    return builder.Finish(
        CanonicalAction::ExecutionObservePausedPc,
        {});
}

ProgramValueGraph InputLeaseRequestGraph()
{
    TestStaticConfigWriter writer({'I', 'L', 'C', '2'});
    writer.U32(0);
    writer.U32(0);
    writer.Bool(true);
    writer.Bool(true);
    writer.Bool(false);
    TestValueGraphBuilder builder;
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputLeaseStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::InputAcquireLease,
        {config});
}

ProgramValueGraph InputStateRequestGraph(
    CanonicalAction action,
    const ProgramValue& lease,
    std::vector<Byte> frame)
{
    TestValueGraphBuilder builder;
    const ProgramValueId lease_id = builder.AddFrom(lease);
    const ProgramValueId frame_id = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload),
        std::move(frame));
    return builder.Finish(
        action,
        {lease_id, frame_id});
}

ProgramValueGraph CompleteInputDeliveryRequestGraph(
    const ProgramValue& lease,
    const ProgramValue& binding)
{
    TestValueGraphBuilder builder;
    const ProgramValueId lease_id = builder.AddFrom(lease);
    const ProgramValueId binding_id = builder.AddFrom(binding);
    return builder.Finish(
        CanonicalAction::InputCompleteDelivery,
        {lease_id, binding_id});
}

struct TestPoint
{
    CapabilityPackIdentity pack;
    SemanticPointDescriptor point;
};

TestPoint FirstRegisteredPcPoint()
{
    const SourceCapabilityPackCatalog catalog =
        BuildSourceCapabilityPackCatalog();
    for (const CapabilityPackManifest& manifest :
         catalog.manifests)
    {
        const auto point = std::ranges::find_if(
            manifest.semantic_points,
            [](const SemanticPointDescriptor& candidate) {
                return candidate.kind ==
                        SemanticPointKind::
                            ProgramCounter &&
                    candidate.pc != 0;
            });
        if (point != manifest.semantic_points.end())
            return {manifest.identity, *point};
    }
    throw std::logic_error("test source pack has no PC point");
}

ProgramValueGraph ContinueRequestGraph(
    const TestPoint& target,
    const ProgramValue* input_binding = nullptr,
    const ProgramValue* playback_session = nullptr,
    std::optional<std::uint64_t> expected_movie_input_count = std::nullopt)
{
    TestStaticConfigWriter points_writer({'S', 'P', 'S', '1'});
    points_writer.U32(1);
    points_writer.String(target.pack.canonical_id);
    points_writer.U32(target.pack.version);
    points_writer.Hash(target.pack.manifest_hash);
    points_writer.String(target.point.canonical_id);
    points_writer.U8(static_cast<std::uint8_t>(
        SemanticPointKind::ProgramCounter));
    points_writer.U32(target.point.pc);
    points_writer.U32(0);

    TestStaticConfigWriter writer({'C', 'U', 'C', '1'});
    writer.U8(1);
    writer.Bool(true);
    writer.U8(0);
    writer.U8(0);
    writer.U8(0);

    TestValueGraphBuilder builder;
    const ProgramValueId points = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::SemanticPointSet),
        points_writer.Finish());
    std::optional<ProgramValueId> binding_value;
    if (input_binding)
    {
        binding_value =
            builder.AddFrom(*input_binding);
    }
    const ProgramValueId binding = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalInputExecutionBinding),
        OptionalValue{binding_value});
    std::optional<ProgramValueId> playback_value;
    if (playback_session)
        playback_value = builder.AddFrom(*playback_session);
    const ProgramValueId playback = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalMoviePlaybackSession),
        OptionalValue{playback_value});
    std::optional<ProgramValueId> expected_count_value;
    if (expected_movie_input_count)
    {
        expected_count_value = builder.Add(
            TypeRef::Builtin(BuiltinType::U64),
            *expected_movie_input_count);
    }
    const ProgramValueId expected_count = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalMovieInputCount),
        OptionalValue{expected_count_value});
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                ContinueUntilStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::ExecutionContinueUntil,
        {points, binding, playback, expected_count, config});
}

void PutU32(
    ScriptedDolphinBackendControl& control,
    std::uint32_t address,
    std::uint32_t value)
{
    control.guest_memory[address] =
        static_cast<std::uint8_t>(value >> 24u);
    control.guest_memory[address + 1] =
        static_cast<std::uint8_t>(value >> 16u);
    control.guest_memory[address + 2] =
        static_cast<std::uint8_t>(value >> 8u);
    control.guest_memory[address + 3] =
        static_cast<std::uint8_t>(value);
}

void PutU16(
    ScriptedDolphinBackendControl& control,
    std::uint32_t address,
    std::uint16_t value)
{
    control.guest_memory[address] =
        static_cast<std::uint8_t>(value >> 8u);
    control.guest_memory[address + 1] =
        static_cast<std::uint8_t>(value);
}

SchemaIdentity SourceSchema(std::string_view canonical_id)
{
    const SourceCapabilityPackCatalog catalog =
        BuildSourceCapabilityPackCatalog();
    const auto found = std::ranges::find_if(
        catalog.schemas,
        [canonical_id](const TypeSchemaDefinition& schema) {
            return schema.identity.canonical_id == canonical_id;
        });
    if (found == catalog.schemas.end())
        throw std::logic_error("source schema is unavailable");
    return found->identity;
}

ProgramValueGraph ContextRequest(
    std::string_view schema,
    WorksetEpoch epoch,
    std::uint32_t pc)
{
    ProgramValue epoch_value{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U64),
        epoch.value()};
    ProgramValue pc_value{
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U32),
        pc};
    ProgramValue root{
        ProgramValueId(3),
        TypeRef::Named(SourceSchema(schema)),
        RecordValue{{epoch_value.id, pc_value.id}}};
    return {
        root.id,
        {std::move(epoch_value),
         std::move(pc_value),
         std::move(root)}};
}

void WriteBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("test artifact could not be opened");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
        throw std::runtime_error("test artifact could not be written");
}

std::vector<std::uint8_t> MakeDtm()
{
    std::vector<std::uint8_t> bytes(256, 0);
    bytes[0] = 'D';
    bytes[1] = 'T';
    bytes[2] = 'M';
    bytes[3] = 0x1a;
    bytes[4] = 'T';
    bytes[5] = 'E';
    bytes[6] = 'S';
    bytes[7] = 'T';
    bytes[8] = '0';
    bytes[9] = '0';
    bytes[11] = 1;
    return bytes;
}

MovieCheckpointMetadata RestoredReadOnlyMovie(
    const std::filesystem::path& dtm,
    std::vector<std::uint8_t> bytes)
{
    MovieCheckpointMetadata movie;
    movie.mode = MovieCheckpointMode::ReadOnlyPlayback;
    movie.dtm_bytes = std::move(bytes);
    movie.dtm_path = dtm;
    movie.current_frame = 37;
    movie.current_input_count = 0;
    movie.cursor_known = true;
    return movie;
}

void PumpHostExecution(HostHarness& harness)
{
    for (int pump = 0;
         pump < 16 &&
         harness.host->snapshot().execution_pending;
         ++pump)
    {
        harness.session.PumpExecution();
        std::vector<ExecutionEvent> events =
            harness.session.DrainExecutionEvents();
        for (ExecutionEvent& event : events)
            harness.host->HandleExecutionEvent(
                std::move(event));
    }
}

TEST(
    SessionProgramActionHost,
    BindsActorLazilyAndRejectsLaterCallsFromAnotherThread)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());

    ProgramActionRequest malformed = harness.Request(
        ProgramHostOperation::PrepareInvocationState,
        harness.session.snapshot().workset_epoch);
    const ProgramActionDispatchResult first =
        harness.host->Dispatch(malformed);
    ASSERT_TRUE(first.immediate_result);
    EXPECT_EQ(
        first.immediate_result->resolution.code,
        "invalid_state_request");

    std::promise<ProgramActionDispatchResult> attempted;
    std::thread other([&] {
        attempted.set_value(
            harness.host->Dispatch(malformed));
    });
    other.join();
    const ProgramActionDispatchResult wrong_thread =
        attempted.get_future().get();
    ASSERT_TRUE(wrong_thread.immediate_result);
    EXPECT_EQ(
        wrong_thread.immediate_result->resolution.code,
        "wrong_thread");

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    AcceptsOnlyPreparedArtifactBaselineState)
{
    TemporaryDirectory temporary;
    HostHarness harness;
    ASSERT_TRUE(harness.Open());

    ProgramActionDispatchResult boot =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "first-battle");
    ASSERT_TRUE(boot.accepted);
    ASSERT_TRUE(boot.immediate_result);
    EXPECT_EQ(
        boot.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    EXPECT_EQ(
        harness.session.snapshot().workset_epoch,
        WorksetEpoch(1));

    CanonicalActionPayload save;
    ASSERT_TRUE(save.AddUtf8(
        CanonicalActionPayloadField::Path,
        temporary.File("baseline.sav").string()));
    ProgramActionDispatchResult saved =
        harness.Invoke(
            CanonicalAction::SavestateSaveImmutableArtifact,
            std::move(save));
    ASSERT_TRUE(saved.accepted);
    ASSERT_TRUE(saved.immediate_result);
    const ProgramValue* artifact_root =
        Root(saved.immediate_result->resolution.output);
    ASSERT_NE(artifact_root, nullptr);
    EXPECT_FALSE(std::holds_alternative<ArtifactReferenceValue>(
        artifact_root->payload));
    EXPECT_FALSE(
        CanonicalActionArtifactReferenceSchemaIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact));
    ASSERT_EQ(
        saved.immediate_result
            ->staged_outputs.size(),
        1u);
    StagedSavestateOutput pending =
        std::get<StagedSavestateOutput>(
            std::move(saved.immediate_result
                          ->staged_outputs.front()));
    SavestateArtifactFinalizer finalizer;
    SavestateArtifactFinalizationRequest finalization;
    finalization.item = {
        WorkerWorksetId(1),
        WorkerWorksetItemId(1),
        0,
        InvocationId(100),
        AttemptId(1)};
    finalization.state_artifact_id =
        pending.capture.artifact;
    finalization.logical_artifact_id =
        pending.artifact_id;
    finalization.state = {
        pending.capture.final_path,
        pending.capture.state_bytes,
        {}};
    ASSERT_TRUE(
        finalizer.Submit(std::move(finalization)).result.ok);
    finalizer.Shutdown();
    auto finalized = finalizer.DrainResults();
    ASSERT_EQ(finalized.size(), 1u);
    ASSERT_TRUE(finalized[0].result.ok)
        << finalized[0].result.message;
    const SavestateFileArtifactReceipt committed =
        harness.session.CommitImmutableSavestateArtifact({
            .artifact = finalized[0].state_artifact_id,
            .state_path = finalized[0].state.path,
            .state_size_bytes = finalized[0].state.size_bytes,
            .state_sha256 = finalized[0].state.sha256,
        });
    ASSERT_TRUE(committed.result.ok)
        << committed.result.message;
    const auto artifact_schema =
        CanonicalActionArtifactPayloadSchemaIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact);
    const auto artifact_hash =
        ContentHash256::FromHex(committed.sha256);
    ASSERT_TRUE(artifact_schema);
    ASSERT_TRUE(artifact_hash);
    const ArtifactReferenceValue artifact_copy{
        pending.artifact_id,
        *artifact_schema,
        *artifact_hash,
        committed.path.string(),
        true};

    ASSERT_TRUE(harness.Finish().accepted);
    ProgramActionDispatchResult continued =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "first-battle");
    ASSERT_TRUE(continued.accepted);
    ASSERT_TRUE(continued.immediate_result);
    EXPECT_EQ(
        continued.immediate_result->resolution.workset_epoch,
        WorksetEpoch(1));
    ASSERT_TRUE(harness.Finish().accepted);

    ProgramActionDispatchResult baseline =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "first-battle");
    ASSERT_TRUE(baseline.accepted);
    ASSERT_TRUE(baseline.immediate_result);
    EXPECT_EQ(
        harness.session.snapshot().workset_epoch,
        WorksetEpoch(1));
    ASSERT_TRUE(harness.Finish().accepted);

    ProgramActionDispatchResult loaded =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "artifact-branch",
            true);
    ASSERT_TRUE(loaded.accepted);
    ASSERT_TRUE(loaded.immediate_result);
    EXPECT_EQ(
        harness.session.snapshot().workset_epoch,
        WorksetEpoch(1));
    EXPECT_EQ(harness.control->restore_file_count, 0);
    ASSERT_TRUE(harness.Finish().accepted);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RejectsExternalMovieStateWithoutExactCompanionMetadata)
{
    TemporaryDirectory temporary;
    const std::filesystem::path state =
        temporary.File("external.sav");
    const std::filesystem::path movie =
        std::filesystem::path(state.string() + ".dtm");
    {
        std::ofstream output(
            state,
            std::ios::binary | std::ios::trunc);
        output << "state";
    }
    {
        std::ofstream output(
            movie,
            std::ios::binary | std::ios::trunc);
        output << "movie";
    }

    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ArtifactReferenceValue external{
        "external-state",
        *CanonicalActionArtifactPayloadSchemaIdentity(
            CanonicalAction::SavestateSaveImmutableArtifact),
        *ContentHash256::FromHex(
            hash::sha256_of_file(state.string())),
        state.string(),
        true};
    ProgramActionDispatchResult rejected =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "external-branch",
            true);
    EXPECT_TRUE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "");
    EXPECT_EQ(harness.control->restore_file_count, 0);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RejectsRestoreBaselineThatWasNotPreparedByTheWorkset)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    const SessionSnapshot before = harness.session.snapshot();
    ProgramActionDispatchResult rejected =
        harness.Prepare(
            InvocationStatePolicy::RestoreBaseline,
            "baseline-lineage",
            false);
    EXPECT_FALSE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "prepared_baseline_required");
    EXPECT_FALSE(
        harness.host->snapshot().invocation_active);
    EXPECT_EQ(
        harness.host->snapshot().mapped_scope_count,
        0u);
    EXPECT_EQ(
        harness.session.snapshot().workset_epoch,
        before.workset_epoch);
    EXPECT_EQ(harness.control->restore_file_count, 0);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    EnforcesBoundedHostDeadlineAndEffectAuthorizationBeforeMutation)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "policy-test").accepted);

    ProgramActionRequest expired = harness.Request(
        ProgramHostOperation::InvokeAction,
        harness.session.snapshot().workset_epoch);
    expired.action =
        CanonicalActionIdentity(CanonicalAction::GuestReadU32);
    expired.timing = ActionTimingClass::BoundedHostOperation;
    expired.bounded_host_deadline =
        std::chrono::steady_clock::now() - 1ms;
    ProgramActionDispatchResult timed_out =
        harness.host->Dispatch(std::move(expired));
    EXPECT_FALSE(timed_out.accepted);
    ASSERT_TRUE(timed_out.immediate_result);
    EXPECT_EQ(
        timed_out.immediate_result->resolution.status,
        ProgramActionResolutionStatus::TimedOut);

    ProgramActionRequest cancellation_driven = harness.Request(
        ProgramHostOperation::InvokeAction,
        harness.session.snapshot().workset_epoch);
    cancellation_driven.action =
        CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntil);
    cancellation_driven.timing =
        ActionTimingClass::CancellationDriven;
    cancellation_driven.bounded_host_deadline =
        std::chrono::steady_clock::now() - 1ms;
    ProgramActionDispatchResult cancellation_driven_result =
        harness.host->Dispatch(std::move(cancellation_driven));
    ASSERT_TRUE(cancellation_driven_result.immediate_result);
    EXPECT_NE(
        cancellation_driven_result.immediate_result->resolution.status,
        ProgramActionResolutionStatus::TimedOut);
    EXPECT_NE(
        cancellation_driven_result.immediate_result->resolution.code,
        "action_deadline");

    ProgramActionRequest unauthorized = harness.Request(
        ProgramHostOperation::InvokeAction,
        harness.session.snapshot().workset_epoch);
    unauthorized.action =
        CanonicalActionIdentity(
            CanonicalAction::GuestWriteData);
    unauthorized.allowed_effects =
        ~EffectMask(ActionEffect::MutateGuest);
    ProgramActionDispatchResult denied =
        harness.host->Dispatch(std::move(unauthorized));
    EXPECT_FALSE(denied.accepted);
    ASSERT_TRUE(denied.immediate_result);
    EXPECT_EQ(
        denied.immediate_result->resolution.code,
        "action_effect_not_authorized");

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RejectsEveryMalformedTypedAdvancePolicyBeforeSessionMutation)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "enum-validation").accepted);

    const std::array<std::array<std::uint8_t, 5>, 5>
        malformed_policies{{
            {255, 0, 0, 0, 0},
            {2, 2, 0, 0, 0},
            {2, 0, 2, 0, 0},
            {2, 0, 0, 255, 0},
            {2, 0, 0, 0, 255},
        }};
    const auto calls_before = harness.control->Calls();
    for (const auto& policy : malformed_policies)
    {
        std::vector<Byte> config{
            static_cast<Byte>('E'),
            static_cast<Byte>('A'),
            static_cast<Byte>('C'),
            static_cast<Byte>('1')};
        config.insert(
            config.end(),
            policy.begin(),
            policy.end());
        ProgramActionDispatchResult rejected =
            harness.InvokeGraph(
                CanonicalAction::ExecutionStepFrames,
                StepFramesRequestGraph(
                    1,
                    std::move(config)));
        EXPECT_FALSE(rejected.accepted);
        ASSERT_TRUE(rejected.immediate_result);
        EXPECT_EQ(
            rejected.immediate_result->resolution.code,
            "invalid_action_payload");
        EXPECT_FALSE(
            harness.host->snapshot().execution_pending);
    }
    const auto calls_after = harness.control->Calls();
    EXPECT_EQ(
        std::ranges::count(
            calls_after,
            std::string("begin_frame_step")),
        std::ranges::count(
            calls_before,
            std::string("begin_frame_step")));
    EXPECT_EQ(
        harness.host->snapshot().mapped_resource_count,
        0u);

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RejectsMalformedTypedRequestBeforeCallingAService)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "typed-request-validation").accepted);

    ProgramValueGraph malformed = StepFramesRequestGraph(1);
    const auto config = std::ranges::find_if(
        malformed.values,
        [](const ProgramValue& value) {
            return value.type ==
                CanonicalRuntimeType(
                    CanonicalRuntimeSchema::
                        ExecutionAdvanceStaticConfig);
        });
    ASSERT_NE(config, malformed.values.end());
    auto* bytes =
        std::get_if<std::vector<Byte>>(&config->payload);
    ASSERT_NE(bytes, nullptr);
    ASSERT_FALSE(bytes->empty());
    (*bytes)[0] = static_cast<Byte>('X');

    const auto before = harness.control->Calls();
    ProgramActionDispatchResult rejected =
        harness.InvokeGraph(
            CanonicalAction::ExecutionStepFrames,
            std::move(malformed));
    EXPECT_FALSE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "invalid_action_payload");
    const auto after = harness.control->Calls();
    EXPECT_EQ(
        std::ranges::count(
            before,
            std::string("begin_frame_step")),
        std::ranges::count(
            after,
            std::string("begin_frame_step")));

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    DirectSemanticPointContinueQueuesTypedReceipt)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "typed-continue").accepted);
    const TestPoint target = FirstRegisteredPcPoint();

    ProgramActionDispatchResult continued =
        harness.InvokeGraph(
            CanonicalAction::ExecutionContinueUntil,
            ContinueRequestGraph(target));
    ASSERT_TRUE(continued.accepted)
        << continued.diagnostic;
    EXPECT_FALSE(continued.immediate_result);
    EXPECT_TRUE(
        harness.host->snapshot().execution_pending);

    savor::probe::INativeStopSink* sink =
        harness.physical_control->BoundSink();
    ASSERT_NE(sink, nullptr);
    const savor::probe::NativeStopDecision decision =
        sink->OnPcStop({
            savor::probe::NativeStopOrigin::Jit,
            target.point.pc,
            nullptr});
    EXPECT_TRUE(decision.request_break);
    harness.control->pc = target.point.pc;
    harness.control->SetCoreState(
        BackendCoreState::Paused);
    std::vector<StopRouteReceipt> receipts =
        harness.session.DrainStopPointEvents();
    ASSERT_FALSE(receipts.empty());
    for (StopRouteReceipt& receipt : receipts)
        harness.session.HandleStopPointReceipt(
            std::move(receipt));
    PumpHostExecution(harness);

    std::vector<ActorActionResult> completions =
        harness.host->DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_EQ(
        completions.front().resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* result =
        Root(completions.front().resolution.output);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(
        result->type,
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil));
    const auto* record =
        std::get_if<RecordValue>(&result->payload);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->fields.size(), 6u);
    const auto optional_stop_iterator = std::ranges::find(
        completions.front().resolution.output.values,
        record->fields[1],
        &ProgramValue::id);
    const ProgramValue* optional_stop =
        optional_stop_iterator ==
            completions.front().resolution.output.values.end()
        ? nullptr
        : &*optional_stop_iterator;
    ASSERT_NE(optional_stop, nullptr);
    const auto* optional = std::get_if<OptionalValue>(
        &optional_stop->payload);
    ASSERT_NE(optional, nullptr);
    ASSERT_TRUE(optional->value.has_value());
    const auto routed_stop_iterator = std::ranges::find(
        completions.front().resolution.output.values,
        *optional->value,
        &ProgramValue::id);
    const ProgramValue* routed_stop =
        routed_stop_iterator == completions.front().resolution.output.values.end()
        ? nullptr
        : &*routed_stop_iterator;
    ASSERT_NE(routed_stop, nullptr);
    const auto* routed_record = std::get_if<RecordValue>(
        &routed_stop->payload);
    ASSERT_NE(routed_record, nullptr);
    ASSERT_EQ(routed_record->fields.size(), 5u);
    const auto evidence_iterator = std::ranges::find(
        completions.front().resolution.output.values,
        routed_record->fields[4],
        &ProgramValue::id);
    const ProgramValue* evidence = evidence_iterator ==
            completions.front().resolution.output.values.end()
        ? nullptr
        : &*evidence_iterator;
    ASSERT_NE(evidence, nullptr);
    const auto* evidence_bytes =
        std::get_if<std::vector<Byte>>(
            &evidence->payload);
    ASSERT_NE(evidence_bytes, nullptr);
    ASSERT_GE(evidence_bytes->size(), 4u);
    EXPECT_EQ(
        std::string(
            evidence_bytes->begin(),
            evidence_bytes->begin() + 4),
        "RSE1");
    // ExecutionEngine owns and releases the foreground registration with the
    // completed operation.
    EXPECT_TRUE(
        harness.session.stop_points()
            ->DesiredPhysicalPlan()
            .pcs.empty());

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    ZeroCompletionCapacityStillReservesOneTerminalSlot)
{
    HostHarness harness;
    SessionProgramActionHostConfig config;
    config.maximum_retained_completions = 0;
    ASSERT_TRUE(harness.Open(config));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "completion-capacity").accepted);

    ProgramActionDispatchResult first =
        harness.InvokeGraph(
            CanonicalAction::ExecutionStepFrames,
            StepFramesRequestGraph(1));
    ASSERT_TRUE(first.accepted);
    EXPECT_FALSE(first.immediate_result);
    PumpHostExecution(harness);
    EXPECT_FALSE(
        harness.host->snapshot().execution_pending);
    EXPECT_EQ(
        harness.host->snapshot().queued_completion_count,
        1u);

    const auto calls_before = harness.control->Calls();
    ProgramActionDispatchResult second =
        harness.InvokeGraph(
            CanonicalAction::ExecutionStepFrames,
            StepFramesRequestGraph(1));
    EXPECT_FALSE(second.accepted);
    ASSERT_TRUE(second.immediate_result);
    EXPECT_EQ(
        second.immediate_result->resolution.code,
        "completion_capacity_exhausted");
    const auto calls_after = harness.control->Calls();
    EXPECT_EQ(
        std::ranges::count(
            calls_after,
            std::string("begin_frame_step")),
        std::ranges::count(
            calls_before,
            std::string("begin_frame_step")));

    std::vector<ActorActionResult> completions =
        harness.host->DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_EQ(
        completions.front().resolution.status,
        ProgramActionResolutionStatus::Completed);
    EXPECT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    MovieStateObservationReturnsServiceOwnedStateWithoutMutation)
{
    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "movie-state-observation").accepted);
    MovieService* movies = harness.session.movie_service();
    ASSERT_NE(movies, nullptr);
    ASSERT_EQ(movies->state(), MovieState::Inactive);
    const WorksetEpoch epoch = harness.session.snapshot().workset_epoch;
    const auto calls_before = harness.control->Calls();

    const ProgramActionDispatchResult observed = harness.InvokeGraph(
        CanonicalAction::MovieObserveState,
        ObserveMovieStateRequestGraph());

    ASSERT_TRUE(observed.accepted) << observed.diagnostic;
    ASSERT_TRUE(observed.immediate_result);
    const auto& output = observed.immediate_result->resolution.output;
    const ProgramValue* root = Root(output);
    ASSERT_NE(root, nullptr);
    const auto* record = std::get_if<RecordValue>(&root->payload);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->fields.size(), 5u);
    const auto value = [&](std::size_t index) -> const ProgramValue* {
        const auto found = std::ranges::find(
            output.values, record->fields[index], &ProgramValue::id);
        return found == output.values.end() ? nullptr : &*found;
    };
    ASSERT_NE(value(0), nullptr);
    const auto* state = std::get_if<EnumValue>(&value(0)->payload);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->value, static_cast<std::int64_t>(MovieState::Inactive));
    ASSERT_NE(value(1), nullptr);
    EXPECT_EQ(std::get<std::uint64_t>(value(1)->payload), epoch.value());
    ASSERT_NE(value(2), nullptr);
    EXPECT_TRUE(std::get<bool>(value(2)->payload));
    ASSERT_NE(value(3), nullptr);
    ASSERT_NE(value(4), nullptr);
    EXPECT_EQ(movies->state(), MovieState::Inactive);
    EXPECT_FALSE(movies->reservation());
    const auto calls_after = harness.control->Calls();
    EXPECT_EQ(calls_after.size(), calls_before.size() + 1u);
    EXPECT_EQ(calls_after.back(), "movie.observe-paused");
    EXPECT_EQ(harness.host->snapshot().mapped_resource_count, 0u);

    EXPECT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    MoviePlaybackPreservesEpochAndReturnsAWorksetBoundHandle)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("playback.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open({}, dtm));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::EstablishBaseline,
        "movie-test",
        false).accepted);
    const WorksetEpoch before =
        harness.session.snapshot().workset_epoch;

    ProgramActionDispatchResult movie_prepared =
        PrepareMoviePlayback(harness, dtm);
    ASSERT_TRUE(movie_prepared.accepted) << movie_prepared.diagnostic;
    ProgramActionDispatchResult started =
        StartPreparedMoviePlayback(harness, movie_prepared);
    ASSERT_TRUE(started.accepted)
        << started.diagnostic;
    ASSERT_TRUE(started.immediate_result);
    EXPECT_EQ(harness.session.snapshot().workset_epoch, before);
    EXPECT_EQ(
        started.immediate_result->resolution.workset_epoch,
        harness.session.snapshot().workset_epoch);
    const ProgramValue* movie_root =
        Root(started.immediate_result->resolution.output);
    ASSERT_NE(movie_root, nullptr);
    const auto* movie_handle =
        std::get_if<ResourceHandleValue>(
            &movie_root->payload);
    ASSERT_NE(movie_handle, nullptr);
    EXPECT_EQ(movie_handle->workset_epoch, before);

    ProgramActionRequest stop = harness.Request(
        ProgramHostOperation::InvokeAction,
        harness.session.snapshot().workset_epoch);
    stop.action = CanonicalActionIdentity(
        CanonicalAction::MovieStopPlayback);
    stop.input = started.immediate_result->resolution.output;
    ProgramActionDispatchResult stopped =
        harness.host->Dispatch(std::move(stop));
    ASSERT_TRUE(stopped.accepted);
    ASSERT_TRUE(stopped.immediate_result);
    ASSERT_EQ(
        stopped.immediate_result->resolution.cleanup_receipts.size(),
        1u);
    EXPECT_EQ(
        stopped.immediate_result->resolution.cleanup_receipts[0].status,
        ProgramCleanupStatus::Clean);

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    AdoptsRestoredReadOnlyPlaybackExactlyOnceWithoutRestartingIt)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("restored-playback.dtm");
    const std::vector<std::uint8_t> bytes = MakeDtm();
    WriteBytes(dtm, bytes);

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open(
        {},
        std::nullopt,
        RestoredReadOnlyMovie(dtm, bytes)));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "restored-playback-adoption").accepted);
    MovieService* movies = harness.session.movie_service();
    ASSERT_NE(movies, nullptr);
    ASSERT_EQ(movies->state(), MovieState::ReadOnlyPlayback);
    const WorksetEpoch epoch = harness.session.snapshot().workset_epoch;
    const MovieReservationId reservation = movies->reservation();
    ASSERT_TRUE(reservation);
    const MovieStateSnapshot cursor = movies->ReconcilePausedState(epoch);
    ASSERT_TRUE(cursor.result.ok) << cursor.result.message;
    const auto calls_before = harness.control->Calls();
    const int core_stops_before = harness.control->core_stop_count;
    const int core_starts_before = harness.control->core_start_count;

    ProgramActionDispatchResult adopted = harness.InvokeGraph(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        AdoptRestoredMoviePlaybackRequestGraph());

    ASSERT_TRUE(adopted.accepted) << adopted.diagnostic;
    ASSERT_TRUE(adopted.immediate_result);
    EXPECT_EQ(
        adopted.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    ASSERT_EQ(adopted.immediate_result->resolution.resources.size(), 1u);
    EXPECT_EQ(
        harness.host->snapshot().mapped_resource_count,
        1u);
    const ProgramValue* playback =
        Root(adopted.immediate_result->resolution.output);
    ASSERT_NE(playback, nullptr);
    EXPECT_EQ(
        playback->type,
        CanonicalActionOutputType(
            CanonicalAction::MovieStartPlayback));
    EXPECT_EQ(
        adopted.immediate_result->resolution.workset_epoch,
        epoch);
    EXPECT_EQ(movies->state(), MovieState::ReadOnlyPlayback);
    EXPECT_EQ(movies->reservation(), reservation);
    const MovieStateSnapshot cursor_after =
        movies->ReconcilePausedState(epoch);
    ASSERT_TRUE(cursor_after.result.ok) << cursor_after.result.message;
    EXPECT_EQ(cursor_after.state, cursor.state);
    EXPECT_EQ(cursor_after.read_only, cursor.read_only);
    EXPECT_EQ(cursor_after.current_frame, cursor.current_frame);
    EXPECT_EQ(
        cursor_after.current_input_count,
        cursor.current_input_count);
    EXPECT_EQ(harness.control->core_stop_count, core_stops_before);
    EXPECT_EQ(harness.control->core_start_count, core_starts_before);
    const auto calls_after_adopt = harness.control->Calls();
    EXPECT_EQ(
        std::ranges::count(
            calls_after_adopt,
            std::string("movie.observe-paused")),
        std::ranges::count(
            calls_before,
            std::string("movie.observe-paused")) + 3);
    EXPECT_EQ(
        std::ranges::count(
            calls_after_adopt,
            std::string("movie.prepare")),
        std::ranges::count(
            calls_before,
            std::string("movie.prepare")));
    EXPECT_EQ(
        std::ranges::count(
            calls_after_adopt,
            std::string("movie.activate")),
        std::ranges::count(
            calls_before,
            std::string("movie.activate")));

    ProgramActionDispatchResult duplicate = harness.InvokeGraph(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        AdoptRestoredMoviePlaybackRequestGraph());
    EXPECT_FALSE(duplicate.accepted);
    ASSERT_TRUE(duplicate.immediate_result);
    EXPECT_EQ(
        duplicate.immediate_result->resolution.code,
        "movie_playback_already_adopted");
    EXPECT_EQ(
        harness.host->snapshot().mapped_resource_count,
        1u);
    EXPECT_EQ(movies->reservation(), reservation);
    const auto calls_after_duplicate = harness.control->Calls();
    EXPECT_EQ(
        std::ranges::count(
            calls_after_duplicate,
            std::string("movie.prepare")),
        std::ranges::count(
            calls_before,
            std::string("movie.prepare")));
    EXPECT_EQ(
        std::ranges::count(
            calls_after_duplicate,
            std::string("movie.activate")),
        std::ranges::count(
            calls_before,
            std::string("movie.activate")));

    ProgramActionDispatchResult finished = harness.Finish();
    ASSERT_TRUE(finished.accepted);
    ASSERT_TRUE(finished.immediate_result);
    ASSERT_EQ(
        finished.immediate_result->resolution.cleanup_receipts.size(),
        1u);
    EXPECT_EQ(
        finished.immediate_result->resolution.cleanup_receipts[0].status,
        ProgramCleanupStatus::Clean);
    EXPECT_EQ(movies->state(), MovieState::Inactive);
    EXPECT_FALSE(movies->reservation());
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("movie.stop")),
        1);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    AdoptedRestoredPlaybackIsAcceptedByMovieRecordingBranch)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("restored-recording-source.dtm");
    const std::filesystem::path output =
        temporary.File("restored-recording-output.dtm");
    const std::vector<std::uint8_t> bytes = MakeDtm();
    WriteBytes(dtm, bytes);

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open(
        {},
        std::nullopt,
        RestoredReadOnlyMovie(dtm, bytes)));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "restored-playback-recording").accepted);
    ProgramActionDispatchResult adopted = harness.InvokeGraph(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        AdoptRestoredMoviePlaybackRequestGraph());
    ASSERT_TRUE(adopted.accepted) << adopted.diagnostic;
    ASSERT_TRUE(adopted.immediate_result);
    const ProgramValue* playback =
        Root(adopted.immediate_result->resolution.output);
    ASSERT_NE(playback, nullptr);
    const int core_stops_before = harness.control->core_stop_count;
    const int core_starts_before = harness.control->core_start_count;

    ProgramActionDispatchResult recording = harness.InvokeGraph(
        CanonicalAction::MovieStartRecording,
        StartMovieRecordingRequestGraph(*playback, output));

    ASSERT_TRUE(recording.accepted) << recording.diagnostic;
    ASSERT_TRUE(recording.immediate_result);
    ASSERT_NE(harness.session.movie_service(), nullptr);
    EXPECT_EQ(
        harness.session.movie_service()->state(),
        MovieState::Recording);
    EXPECT_EQ(harness.control->core_stop_count, core_stops_before);
    EXPECT_EQ(harness.control->core_start_count, core_starts_before);
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("movie.branch-playback-to-recording")),
        1);
    EXPECT_EQ(
        harness.host->snapshot().mapped_resource_count,
        1u);

    EXPECT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    AdoptedPlaybackNaturalEndRetainsCheckpointUntilHostOnlyCleanup)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("restored-playback-ended.dtm");
    const std::vector<std::uint8_t> bytes = MakeDtm();
    WriteBytes(dtm, bytes);

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open(
        {},
        std::nullopt,
        RestoredReadOnlyMovie(dtm, bytes)));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "restored-playback-ended").accepted);
    ProgramActionDispatchResult adopted = harness.InvokeGraph(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        AdoptRestoredMoviePlaybackRequestGraph());
    ASSERT_TRUE(adopted.accepted) << adopted.diagnostic;

    MovieService* movies = harness.session.movie_service();
    ASSERT_NE(movies, nullptr);
    const WorksetEpoch epoch = harness.session.snapshot().workset_epoch;
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->movie_observation.playing = false;
        harness.control->movie_observation.recording = false;
        harness.control->movie_observation.read_only = true;
        harness.control->movie_observation.current_frame = 41;
        harness.control->movie_observation.current_input_count = 3;
    }
    const MovieStateSnapshot ended = movies->ReconcilePausedState(epoch);
    ASSERT_TRUE(ended.result.ok) << ended.result.message;
    EXPECT_EQ(ended.state, MovieState::PlaybackEnded);
    EXPECT_EQ(ended.current_frame, 41u);
    EXPECT_EQ(ended.current_input_count, 3u);

    const MovieCheckpointReceipt checkpoint = movies->CaptureCheckpoint();
    ASSERT_TRUE(checkpoint.result.ok) << checkpoint.result.message;
    ASSERT_TRUE(checkpoint.checkpoint);
    EXPECT_EQ(
        checkpoint.checkpoint->mode,
        MovieCheckpointMode::ReadOnlyPlayback);
    EXPECT_EQ(checkpoint.checkpoint->current_frame, 41u);
    EXPECT_EQ(checkpoint.checkpoint->current_input_count, 3u);

    ASSERT_TRUE(harness.Finish().accepted);
    EXPECT_EQ(movies->state(), MovieState::Inactive);
    EXPECT_FALSE(movies->reservation());
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("movie.stop")),
        0);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RejectsPlaybackAdoptionWithoutARestoredMovie)
{
    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "missing-restored-playback").accepted);

    ProgramActionDispatchResult adopted = harness.InvokeGraph(
        CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
        AdoptRestoredMoviePlaybackRequestGraph());

    EXPECT_FALSE(adopted.accepted);
    ASSERT_TRUE(adopted.immediate_result);
    EXPECT_EQ(
        adopted.immediate_result->resolution.code,
        "restored_movie_playback_unavailable");
    EXPECT_EQ(
        harness.host->snapshot().mapped_resource_count,
        0u);
    EXPECT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RecordingStartWithoutARegistrableScopeIsCompensatedImmediately)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm = temporary.File("branch-source.dtm");
    const std::filesystem::path output = temporary.File("recording.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open({}, dtm));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::EstablishBaseline,
        "recording-registration-compensation",
        false).accepted);
    ProgramActionDispatchResult prepared = PrepareMoviePlayback(harness, dtm);
    ASSERT_TRUE(prepared.accepted) << prepared.diagnostic;
    ProgramActionDispatchResult playback =
        StartPreparedMoviePlayback(harness, prepared);
    ASSERT_TRUE(playback.accepted) << playback.diagnostic;
    ASSERT_TRUE(playback.immediate_result);
    const ProgramValue* playback_handle =
        Root(playback.immediate_result->resolution.output);
    ASSERT_NE(playback_handle, nullptr);

    ProgramActionDispatchResult recording = harness.InvokeGraph(
        CanonicalAction::MovieStartRecording,
        StartMovieRecordingRequestGraph(*playback_handle, output),
        ProgramScopeId(999));

    EXPECT_FALSE(recording.accepted);
    ASSERT_TRUE(recording.immediate_result);
    EXPECT_EQ(recording.immediate_result->resolution.code,
              "resource_registration_failed");
    ASSERT_NE(harness.session.movie_service(), nullptr);
    EXPECT_EQ(harness.session.movie_service()->state(),
              MovieState::Inactive);
    const auto calls = harness.control->Calls();
    EXPECT_EQ(std::ranges::count(
        calls, std::string("movie.branch-playback-to-recording")), 1);
    EXPECT_EQ(std::ranges::count(
        calls, std::string("movie.cancel-recording")), 1);

    EXPECT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    RequiresExactPausedPcWithoutAdvancingEmulation)
{
    HostHarness harness;
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->pc = 0x80101e48u;
        harness.control->vi_count = 417;
    }
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "battle-entry").accepted);

    const std::size_t calls_before =
        harness.control->Calls().size();
    const ProgramActionDispatchResult mismatch =
        harness.InvokeGraph(
            CanonicalAction::ExecutionRequirePausedPc,
            RequirePausedPcRequestGraph(0x800715dcu));
    ASSERT_TRUE(mismatch.immediate_result);
    EXPECT_EQ(
        mismatch.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Failed);
    EXPECT_EQ(
        mismatch.immediate_result->resolution.code,
        "paused_pc_mismatch");
    EXPECT_EQ(harness.control->Calls().size(), calls_before);

    const ProgramActionDispatchResult matched =
        harness.InvokeGraph(
            CanonicalAction::ExecutionRequirePausedPc,
            RequirePausedPcRequestGraph(0x80101e48u));
    ASSERT_TRUE(matched.immediate_result);
    EXPECT_EQ(
        matched.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* receipt = Root(
        matched.immediate_result->resolution.output);
    ASSERT_NE(receipt, nullptr);
    const auto* fields = std::get_if<RecordValue>(
        &receipt->payload);
    ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->fields.size(), 3u);
    EXPECT_EQ(harness.control->Calls().size(), calls_before);

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    ObservesAuthoritativePausedPcWithoutAdvancingOrPublishingInput)
{
    HostHarness harness;
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->pc = 0x80101894u;
        harness.control->vi_count = 912;
    }
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "seedprobe-entry").accepted);
    const auto calls_before = harness.control->Calls().size();

    const auto observed = harness.InvokeGraph(
        CanonicalAction::ExecutionObservePausedPc,
        ObservePausedPcRequestGraph());
    ASSERT_TRUE(observed.immediate_result);
    EXPECT_EQ(observed.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* receipt = Root(
        observed.immediate_result->resolution.output);
    ASSERT_NE(receipt, nullptr);
    const auto* record = std::get_if<RecordValue>(&receipt->payload);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->fields.size(), 3u);
    const auto& values =
        observed.immediate_result->resolution.output.values;
    const auto pc = std::ranges::find(
        values, record->fields[0], &ProgramValue::id);
    ASSERT_NE(pc, values.end());
    EXPECT_EQ(std::get<std::uint32_t>(pc->payload), 0x80101894u);
    EXPECT_EQ(harness.control->Calls().size(), calls_before);

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SourceCapabilityPacks,
    BattlePredicateSurfaceKeepsStartTurnAndExcludesStartAction)
{
    const SourceCapabilityPackCatalog catalog =
        BuildSourceCapabilityPackCatalog();
    const CapabilityPackIdentity battle = BattlePackIdentity();
    const auto manifest = std::ranges::find(
        catalog.manifests,
        battle,
        &CapabilityPackManifest::identity);
    ASSERT_NE(manifest, catalog.manifests.end());
    const auto has_point = [&](std::string_view suffix) {
        return std::ranges::any_of(
            manifest->semantic_points,
            [&](const SemanticPointDescriptor& point) {
                return point.canonical_id.ends_with(suffix);
            });
    };
    EXPECT_TRUE(has_point(".point.StartTurn"));
    EXPECT_FALSE(has_point(".point.StartAction"));
}

TEST(
    SessionProgramActionHost,
    OwnedPlaybackContinueRequiresExactHandleAndReturnsTypedMovieEnd)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("owned-playback.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open({}, dtm));
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::EstablishBaseline,
        "owned-playback-continue",
        false).accepted);
    ProgramActionDispatchResult movie_prepared =
        PrepareMoviePlayback(harness, dtm);
    ASSERT_TRUE(movie_prepared.accepted) << movie_prepared.diagnostic;
    ProgramActionDispatchResult started =
        StartPreparedMoviePlayback(harness, movie_prepared);
    ASSERT_TRUE(started.accepted) << started.diagnostic;
    ASSERT_TRUE(started.immediate_result);
    const ProgramValue* movie_handle =
        Root(started.immediate_result->resolution.output);
    ASSERT_NE(movie_handle, nullptr);

    const TestPoint target = FirstRegisteredPcPoint();
    ProgramActionDispatchResult missing_handle = harness.InvokeGraph(
        CanonicalAction::ExecutionContinueUntil,
        ContinueRequestGraph(target, nullptr, nullptr, 5));
    EXPECT_FALSE(missing_handle.accepted);

    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->movie_observation.playing = false;
        harness.control->movie_observation.recording = false;
        harness.control->movie_observation.read_only = true;
        harness.control->movie_observation.current_input_count = 5;
    }
    ProgramActionDispatchResult continued = harness.InvokeGraph(
        CanonicalAction::ExecutionContinueUntil,
        ContinueRequestGraph(target, nullptr, movie_handle, 5));
    ASSERT_TRUE(continued.accepted) << continued.diagnostic;
    PumpHostExecution(harness);
    std::vector<ActorActionResult> completions =
        harness.host->DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    ASSERT_EQ(
        completions.front().resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* result = Root(completions.front().resolution.output);
    ASSERT_NE(result, nullptr);
    const auto* record = std::get_if<RecordValue>(&result->payload);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->fields.size(), 6u);
    const auto reason_iterator = std::ranges::find(
        completions.front().resolution.output.values,
        record->fields[0],
        &ProgramValue::id);
    ASSERT_NE(reason_iterator, completions.front().resolution.output.values.end());
    const auto* reason = std::get_if<EnumValue>(
        &reason_iterator->payload);
    ASSERT_NE(reason, nullptr);
    EXPECT_EQ(
        reason->value,
        static_cast<std::int64_t>(
            ContinueUntilCompletionReasonV1::MovieEnded));
    const auto stop_iterator = std::ranges::find(
        completions.front().resolution.output.values,
        record->fields[1],
        &ProgramValue::id);
    ASSERT_NE(stop_iterator, completions.front().resolution.output.values.end());
    const auto* optional_stop = std::get_if<OptionalValue>(
        &stop_iterator->payload);
    ASSERT_NE(optional_stop, nullptr);
    EXPECT_FALSE(optional_stop->value.has_value());

    ASSERT_NE(harness.session.movie_service(), nullptr);
    EXPECT_EQ(
        harness.session.movie_service()->state(),
        MovieState::PlaybackEnded);
    ASSERT_TRUE(harness.Finish().accepted);
    EXPECT_EQ(
        harness.session.movie_service()->state(),
        MovieState::Inactive);
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("movie.stop")),
        0);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    EstablishBaselineRequiresPlaybackBeforeGuestExecution)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("establish-baseline.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open({}, dtm));
    ProgramActionDispatchResult prepared = harness.Prepare(
        InvocationStatePolicy::EstablishBaseline,
        "read-only-movie",
        false);
    ASSERT_TRUE(prepared.accepted);
    ASSERT_TRUE(prepared.immediate_result);

    const TestPoint target = FirstRegisteredPcPoint();
    ProgramActionDispatchResult early_execution = harness.InvokeGraph(
        CanonicalAction::ExecutionContinueUntil,
        ContinueRequestGraph(target));
    EXPECT_FALSE(early_execution.accepted);

    ProgramActionDispatchResult movie_prepared =
        PrepareMoviePlayback(harness, dtm);
    ASSERT_TRUE(movie_prepared.accepted) << movie_prepared.diagnostic;
    ASSERT_TRUE(movie_prepared.immediate_result);
    ProgramActionDispatchResult early_return = harness.Finish();
    EXPECT_FALSE(early_return.accepted);
    ASSERT_TRUE(early_return.immediate_result);
    EXPECT_EQ(
        early_return.immediate_result->resolution.code,
        "baseline_not_established");

    ProgramActionDispatchResult started =
        StartPreparedMoviePlayback(harness, movie_prepared);
    ASSERT_TRUE(started.accepted) << started.diagnostic;
    ASSERT_TRUE(started.immediate_result);
    EXPECT_EQ(harness.control->core_stop_count, 1u);
    EXPECT_EQ(harness.control->core_start_count, 1u);

    ProgramActionDispatchResult finished = harness.Finish();
    ASSERT_TRUE(finished.accepted) << finished.diagnostic;
    ASSERT_TRUE(finished.immediate_result);
    EXPECT_EQ(
        finished.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);

    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    HeldInputReturnsExactExecutionBindingConsumedByAdvancement)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "exact-input-binding").accepted);

    ProgramActionDispatchResult leased =
        harness.InvokeGraph(
            CanonicalAction::InputAcquireLease,
            InputLeaseRequestGraph());
    ASSERT_TRUE(leased.accepted);
    ASSERT_TRUE(leased.immediate_result);
    const ProgramValue* lease =
        Root(leased.immediate_result->resolution.output);
    ASSERT_NE(lease, nullptr);

    const std::vector<Byte> frame{
        0x34,
        0x12,
        128,
        128,
        128,
        128,
        0,
        0};

    ProgramActionDispatchResult dispatched =
        harness.InvokeGraph(
            CanonicalAction::InputApplyState,
            InputStateRequestGraph(
                CanonicalAction::InputApplyState,
                *lease,
                frame));
    ASSERT_TRUE(dispatched.accepted)
        << dispatched.diagnostic;
    ASSERT_TRUE(dispatched.immediate_result);
    ASSERT_EQ(
        dispatched.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* binding =
        Root(dispatched.immediate_result->resolution.output);
    ASSERT_NE(binding, nullptr);

    CanonicalActionPayload receipt;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        dispatched.immediate_result->resolution.output,
        *CanonicalActionOutputSchemaIdentity(
            CanonicalAction::InputApplyState),
        receipt));
    EXPECT_TRUE(receipt.Unsigned(
        CanonicalActionPayloadField::Handle));
    EXPECT_TRUE(receipt.Unsigned(
            CanonicalActionPayloadField::Publication));
    EXPECT_TRUE(receipt.Unsigned(
            CanonicalActionPayloadField::Binding));
    EXPECT_TRUE(receipt.Unsigned(
            CanonicalActionPayloadField::StateGeneration));
    EXPECT_EQ(
        receipt.Unsigned(
            CanonicalActionPayloadField::ResultEpoch),
        harness.session.snapshot().workset_epoch.value());
    ASSERT_TRUE(receipt.Bytes(
        CanonicalActionPayloadField::ResultFrame));
    EXPECT_TRUE(std::ranges::equal(
        *receipt.Bytes(
            CanonicalActionPayloadField::ResultFrame),
        frame));

    ProgramActionDispatchResult stepped =
        harness.InvokeGraph(
            CanonicalAction::ExecutionStepFrames,
            StepFramesRequestGraph(1, {}, binding));
    ASSERT_TRUE(stepped.accepted) << stepped.diagnostic;
    EXPECT_FALSE(stepped.immediate_result);
    ASSERT_TRUE(harness.session.execution_snapshot());
    EXPECT_TRUE(
        harness.session.execution_snapshot()->input_bound);
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->input_callback_count = 1;
    }
    PumpHostExecution(harness);
    const std::vector<ActorActionResult> completions =
        harness.host->DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_EQ(
        completions.front().resolution.status,
        ProgramActionResolutionStatus::Completed);

    CanonicalActionPayload forged;
    ASSERT_TRUE(forged.AddUnsigned(
        CanonicalActionPayloadField::Handle,
        *receipt.Unsigned(
            CanonicalActionPayloadField::Handle) + 1));
    ASSERT_TRUE(forged.AddUnsigned(
        CanonicalActionPayloadField::Publication,
        *receipt.Unsigned(
            CanonicalActionPayloadField::Publication)));
    ASSERT_TRUE(forged.AddUnsigned(
        CanonicalActionPayloadField::Binding,
        *receipt.Unsigned(
            CanonicalActionPayloadField::Binding)));
    ASSERT_TRUE(forged.AddUnsigned(
        CanonicalActionPayloadField::ResultEpoch,
        *receipt.Unsigned(
            CanonicalActionPayloadField::ResultEpoch)));
    ASSERT_TRUE(forged.AddUnsigned(
        CanonicalActionPayloadField::StateGeneration,
        *receipt.Unsigned(
            CanonicalActionPayloadField::StateGeneration)));
    ASSERT_TRUE(forged.AddBytes(
        CanonicalActionPayloadField::ResultFrame,
        std::vector<Byte>(
            receipt.Bytes(
                CanonicalActionPayloadField::ResultFrame)
                ->begin(),
            receipt.Bytes(
                CanonicalActionPayloadField::ResultFrame)
                ->end())));
    CanonicalActionPayloadResult forged_graph =
        EncodeCanonicalActionPayload(
            forged,
            *CanonicalActionOutputSchemaIdentity(
                CanonicalAction::InputApplyState));
    ASSERT_TRUE(forged_graph.ok);
    const ProgramValue* forged_binding =
        Root(forged_graph.graph);
    ASSERT_NE(forged_binding, nullptr);
    ProgramActionDispatchResult rejected =
        harness.InvokeGraph(
            CanonicalAction::ExecutionStepFrames,
            StepFramesRequestGraph(1, {}, forged_binding));
    EXPECT_FALSE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "input_binding_invalid");

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    NeutralOneShotDeliveryPublishesOnceAndReturnsOneTypedReceipt)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "neutral-one-shot-delivery").accepted);

    ProgramActionDispatchResult leased = harness.InvokeGraph(
        CanonicalAction::InputAcquireLease,
        InputLeaseRequestGraph());
    ASSERT_TRUE(leased.accepted);
    ASSERT_TRUE(leased.immediate_result);
    const ProgramValue* lease =
        Root(leased.immediate_result->resolution.output);
    ASSERT_NE(lease, nullptr);

    const std::size_t publications_before = std::ranges::count(
        harness.control->Calls(), std::string("input.publish"));
    ProgramActionDispatchResult begun = harness.InvokeGraph(
        CanonicalAction::InputBeginDelivery,
        InputStateRequestGraph(
            CanonicalAction::InputBeginDelivery,
            *lease,
            std::vector<Byte>{0, 0, 128, 128, 128, 128, 0, 0}));
    ASSERT_TRUE(begun.accepted) << begun.diagnostic;
    ASSERT_TRUE(begun.immediate_result);
    const ProgramValue* binding =
        Root(begun.immediate_result->resolution.output);
    ASSERT_NE(binding, nullptr);

    ProgramActionDispatchResult stepped = harness.InvokeGraph(
        CanonicalAction::ExecutionStepFrames,
        StepFramesRequestGraph(1, {}, binding));
    ASSERT_TRUE(stepped.accepted) << stepped.diagnostic;
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->input_callback_count = 1;
    }
    PumpHostExecution(harness);
    ASSERT_EQ(harness.host->DrainResults().size(), 1u);

    ProgramActionDispatchResult completed = harness.InvokeGraph(
        CanonicalAction::InputCompleteDelivery,
        CompleteInputDeliveryRequestGraph(*lease, *binding));
    ASSERT_TRUE(completed.accepted) << completed.diagnostic;
    ASSERT_TRUE(completed.immediate_result);
    CanonicalActionPayload receipt;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        completed.immediate_result->resolution.output,
        *CanonicalActionOutputSchemaIdentity(
            CanonicalAction::InputCompleteDelivery),
        receipt));
    EXPECT_TRUE(receipt.Unsigned(
        CanonicalActionPayloadField::DeliveryId));
    EXPECT_TRUE(receipt.Unsigned(
        CanonicalActionPayloadField::ResultSequence));
    EXPECT_EQ(receipt.Unsigned(
        CanonicalActionPayloadField::CompletedCount), 1u);
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("input.publish")),
        publications_before + 1);

    ASSERT_TRUE(harness.Finish().accepted);
    EXPECT_EQ(
        std::ranges::count(
            harness.control->Calls(),
            std::string("input.publish")),
        publications_before + 1);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    ExecutesRegisteredNavigationContextAsOneCoherentQuery)
{
    HostHarness harness;
    harness.control->pc = soa::navigation::ctx::CapturePc;
    constexpr std::uint32_t worksheet = 0x80400000u;
    PutU32(
        *harness.control,
        soa::navigation::ctx::PlayerWorksheetPointerAddress,
        worksheet);
    PutU32(
        *harness.control,
        soa::navigation::ctx::AreaAddress,
        201u);
    harness.control->guest_memory[
        soa::navigation::ctx::SubareaAddress] = 'a';
    PutU16(*harness.control, worksheet + 0x170u, 1u);
    PutU16(*harness.control, worksheet + 0x172u, 2u);
    PutU32(
        *harness.control,
        worksheet +
            soa::navigation::ctx::GroundSelectorPointerOffset,
        0u);
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "navigation-query").accepted);
    harness.control->SetCoreState(BackendCoreState::Paused);

    const WorksetEpoch epoch =
        harness.session.snapshot().workset_epoch;
    ProgramActionRequest wrong_pc = harness.Request(
        ProgramHostOperation::InvokeAction,
        epoch);
    wrong_pc.action = NavigationCaptureContextActionIdentity();
    wrong_pc.input = ContextRequest(
        "soa.navigation.CaptureContextRequest",
        epoch,
        soa::navigation::ctx::CapturePc + 4);
    ProgramActionDispatchResult wrong_pc_result =
        harness.host->Dispatch(std::move(wrong_pc));
    EXPECT_FALSE(wrong_pc_result.accepted);
    ASSERT_TRUE(wrong_pc_result.immediate_result);
    EXPECT_EQ(
        wrong_pc_result.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Failed);
    EXPECT_EQ(
        wrong_pc_result.immediate_result->resolution.code,
        "observation_point_unavailable");

    ProgramActionRequest wrong_epoch = harness.Request(
        ProgramHostOperation::InvokeAction,
        epoch);
    wrong_epoch.action = NavigationCaptureContextActionIdentity();
    wrong_epoch.input = ContextRequest(
        "soa.navigation.CaptureContextRequest",
        WorksetEpoch(epoch.value() + 1),
        soa::navigation::ctx::CapturePc);
    ProgramActionDispatchResult wrong_epoch_result =
        harness.host->Dispatch(std::move(wrong_epoch));
    EXPECT_FALSE(wrong_epoch_result.accepted);
    ASSERT_TRUE(wrong_epoch_result.immediate_result);
    EXPECT_EQ(
        wrong_epoch_result.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Rejected);
    EXPECT_EQ(
        wrong_epoch_result.immediate_result->resolution.code,
        "invalid_context_request");

    ProgramActionRequest query = harness.Request(
        ProgramHostOperation::InvokeAction,
        epoch);
    query.action =
        NavigationCaptureContextActionIdentity();
    query.input = ContextRequest(
        "soa.navigation.CaptureContextRequest",
        epoch,
        soa::navigation::ctx::CapturePc);
    ProgramActionDispatchResult captured =
        harness.host->Dispatch(std::move(query));
    ASSERT_TRUE(captured.accepted);
    ASSERT_TRUE(captured.immediate_result);
    EXPECT_EQ(
        captured.immediate_result->resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* context =
        Root(captured.immediate_result->resolution.output);
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(context->type.named.has_value());
    EXPECT_EQ(
        context->type.named->canonical_id,
        "soa.navigation.NavigationContext");

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

} // namespace
