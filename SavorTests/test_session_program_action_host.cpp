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

    bool Open(SessionProgramActionHostConfig config = {})
    {
        const SessionOperationReceipt opened =
            session.Open({});
        if (!opened.ok)
            return false;
        const SessionOperationReceipt begun =
            session.BeginWorkset(WorkerWorksetId(1));
        if (!begun.ok)
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

ProgramValueGraph StepFramesRequestGraph(
    std::uint64_t count,
    std::vector<Byte> config = {})
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
    const ProgramValueId witness = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalInputNeutralWitness),
        OptionalValue{});
    const ProgramValueId static_config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                ExecutionAdvanceStaticConfig),
        std::move(config));
    return builder.Finish(
        CanonicalAction::ExecutionStepFrames,
        {count_id, witness, static_config});
}

ProgramValueGraph InputLeaseRequestGraph(
    bool require_neutral_acknowledgement)
{
    TestStaticConfigWriter writer({'I', 'L', 'C', '1'});
    writer.U32(0);
    writer.U32(0);
    writer.Bool(true);
    writer.Bool(true);
    writer.Bool(require_neutral_acknowledgement);
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

ProgramValueGraph InputSequenceRequestGraph(
    const ProgramValue& lease,
    std::vector<Byte> frames)
{
    TestStaticConfigWriter writer({'I', 'P', 'C', '1'});
    writer.U8(3);
    writer.U8(0);
    TestValueGraphBuilder builder;
    const ProgramValueId lease_id = builder.AddFrom(lease);
    const ProgramValueId frames_id = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputSequencePayload),
        std::move(frames));
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                InputPublicationStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::InputPublishSequence,
        {lease_id, frames_id, config});
}

ProgramValueGraph InputPulseRequestGraph(
    const ProgramValue& lease,
    std::vector<Byte> frame)
{
    TestStaticConfigWriter writer({'I', 'P', 'C', '1'});
    writer.U8(1);
    writer.U8(0);
    TestValueGraphBuilder builder;
    const ProgramValueId lease_id = builder.AddFrom(lease);
    const ProgramValueId frame_id = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputFramePayload),
        std::move(frame));
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                InputPublicationStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::InputPublishPulse,
        {lease_id, frame_id, config});
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
                        SemanticPointPhysicalKind::
                            ProgramCounter &&
                    candidate.pc != 0;
            });
        if (point != manifest.semantic_points.end())
            return {manifest.identity, *point};
    }
    throw std::logic_error("test source pack has no PC point");
}

ProgramValueGraph SubscribeRequestGraph(
    const TestPoint& target)
{
    TestStaticConfigWriter writer({'S', 'G', 'C', '1'});
    writer.U32(1);
    writer.String(target.pack.canonical_id);
    writer.U32(target.pack.version);
    writer.Hash(target.pack.manifest_hash);
    writer.String(target.point.canonical_id);
    writer.U8(static_cast<std::uint8_t>(
        SemanticPointPhysicalKind::ProgramCounter));
    writer.U32(target.point.pc);
    writer.U32(0);
    writer.U8(static_cast<std::uint8_t>(
        StopDeliveryMode::Observe));
    writer.U8(static_cast<std::uint8_t>(
        StopRoutingPolicy::Pass));
    writer.U8(static_cast<std::uint8_t>(
        StopSubscriptionLifetime::Scoped));

    TestValueGraphBuilder builder;
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopGroupStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::StopPointsSubscribeGroup,
        {config});
}

ProgramValueGraph ContinueRequestGraph(
    const ProgramValue& stop_group,
    const ProgramValue* input_publication = nullptr,
    const ProgramValue* playback_session = nullptr,
    std::optional<std::uint64_t> expected_movie_input_count = std::nullopt)
{
    TestStaticConfigWriter writer({'C', 'U', 'C', '1'});
    writer.U8(1);
    writer.Bool(true);
    writer.U8(0);
    writer.U8(0);
    writer.U8(0);

    TestValueGraphBuilder builder;
    const ProgramValueId group = builder.AddFrom(stop_group);
    std::optional<ProgramValueId> publication_value;
    if (input_publication)
    {
        publication_value =
            builder.AddFrom(*input_publication);
    }
    const ProgramValueId publication = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::
                OptionalInputPublicationReceipt),
        OptionalValue{publication_value});
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
        {group, publication, playback, expected_count, config});
}

ProgramValueGraph InputPollRequestGraph(
    const ProgramValue& lease,
    const ProgramValue& publication)
{
    TestStaticConfigWriter writer({'I', 'G', 'P', '1'});
    writer.Bool(false);
    writer.String("test-publication");
    writer.U32(1);
    writer.Bool(true);

    TestValueGraphBuilder builder;
    const ProgramValueId lease_id =
        builder.AddFrom(lease);
    const ProgramValueId publication_value =
        builder.AddFrom(publication);
    const ProgramValueId optional_publication =
        builder.Add(
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt),
            OptionalValue{publication_value});
    const ProgramValueId optional_witness =
        builder.Add(
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness),
            OptionalValue{});
    const ProgramValueId config = builder.Add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::InputPollStaticConfig),
        writer.Finish());
    return builder.Finish(
        CanonicalAction::InputAwaitGuestPoll,
        {
            lease_id,
            optional_publication,
            optional_witness,
            config});
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
    ProgramValue sequence{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{1}};
    ProgramValue epoch_value{
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U64),
        epoch.value()};
    ProgramValue pc_value{
        ProgramValueId(3),
        TypeRef::Builtin(BuiltinType::U32),
        pc};
    ProgramValue root{
        ProgramValueId(4),
        TypeRef::Named(SourceSchema(schema)),
        RecordValue{{sequence.id, epoch_value.id, pc_value.id}}};
    return {
        root.id,
        {std::move(sequence),
         std::move(epoch_value),
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

void AcknowledgeInputAndPumpOnce(HostHarness& harness)
{
    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->input_callback_count = 1;
    }
    harness.session.PumpExecution();
    std::vector<ExecutionEvent> events =
        harness.session.DrainExecutionEvents();
    for (ExecutionEvent& event : events)
        harness.host->HandleExecutionEvent(
            std::move(event));
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
    PromotesRetainedPassiveGroupForContinueAndQueuesTypedReceipt)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "typed-continue").accepted);
    const TestPoint target = FirstRegisteredPcPoint();

    ProgramActionDispatchResult subscribed =
        harness.InvokeGraph(
            CanonicalAction::StopPointsSubscribeGroup,
            SubscribeRequestGraph(target));
    ASSERT_TRUE(subscribed.accepted);
    ASSERT_TRUE(subscribed.immediate_result);
    const ProgramValue* group =
        Root(subscribed.immediate_result->resolution.output);
    ASSERT_NE(group, nullptr);
    ASSERT_NE(
        std::get_if<ResourceHandleValue>(&group->payload),
        nullptr);
    ASSERT_NE(harness.session.stop_points(), nullptr);
    ASSERT_EQ(
        harness.session.stop_points()
            ->DesiredPhysicalPlan()
            .pcs.size(),
        1u);

    ProgramActionDispatchResult continued =
        harness.InvokeGraph(
            CanonicalAction::ExecutionContinueUntil,
            ContinueRequestGraph(*group));
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
    ASSERT_EQ(record->fields.size(), 5u);
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
    // ExecutionEngine released only its temporary Wake group; the passive
    // source-scoped group and its physical union remain live.
    ASSERT_EQ(
        harness.session.stop_points()
            ->DesiredPhysicalPlan()
            .pcs.size(),
        1u);
    EXPECT_EQ(
        harness.session.stop_points()
            ->DesiredPhysicalPlan()
            .pcs.front()
            .pc,
        target.point.pc);

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
    MoviePlaybackPreservesEpochAndReturnsAWorksetBoundHandle)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("playback.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open());
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
    OwnedPlaybackContinueRequiresExactHandleAndReturnsTypedMovieEnd)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("owned-playback.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open());
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
    ProgramActionDispatchResult subscribed = harness.InvokeGraph(
        CanonicalAction::StopPointsSubscribeGroup,
        SubscribeRequestGraph(target));
    ASSERT_TRUE(subscribed.accepted);
    ASSERT_TRUE(subscribed.immediate_result);
    const ProgramValue* group =
        Root(subscribed.immediate_result->resolution.output);
    ASSERT_NE(group, nullptr);

    ProgramActionDispatchResult missing_handle = harness.InvokeGraph(
        CanonicalAction::ExecutionContinueUntil,
        ContinueRequestGraph(*group, nullptr, nullptr, 5));
    EXPECT_FALSE(missing_handle.accepted);

    {
        std::lock_guard lock(harness.control->mutex);
        harness.control->movie_state = BackendMovieState::Ended;
        harness.control->movie_input_count = 5;
    }
    ProgramActionDispatchResult continued = harness.InvokeGraph(
        CanonicalAction::ExecutionContinueUntil,
        ContinueRequestGraph(*group, nullptr, movie_handle, 5));
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
    ASSERT_EQ(record->fields.size(), 5u);
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

    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    EstablishBaselineRequiresPreparationBeforePassiveStopsAndPlayback)
{
    TemporaryDirectory temporary;
    const std::filesystem::path dtm =
        temporary.File("establish-baseline.dtm");
    WriteBytes(dtm, MakeDtm());

    HostHarness harness;
    harness.control->movie_available = true;
    ASSERT_TRUE(harness.Open());
    ProgramActionDispatchResult prepared = harness.Prepare(
        InvocationStatePolicy::EstablishBaseline,
        "read-only-movie",
        false);
    ASSERT_TRUE(prepared.accepted);
    ASSERT_TRUE(prepared.immediate_result);

    const TestPoint target = FirstRegisteredPcPoint();
    ProgramActionDispatchResult early_subscription = harness.InvokeGraph(
        CanonicalAction::StopPointsSubscribeGroup,
        SubscribeRequestGraph(
            target));
    EXPECT_FALSE(early_subscription.accepted);

    ProgramActionDispatchResult movie_prepared =
        PrepareMoviePlayback(harness, dtm);
    ASSERT_TRUE(movie_prepared.accepted) << movie_prepared.diagnostic;
    ASSERT_TRUE(movie_prepared.immediate_result);
    ProgramActionDispatchResult subscribed = harness.InvokeGraph(
        CanonicalAction::StopPointsSubscribeGroup,
        SubscribeRequestGraph(target));
    ASSERT_TRUE(subscribed.accepted) << subscribed.diagnostic;

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
    RejectedExecutionCompensatesItsInputAdvanceBinding)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "input-compensation").accepted);
    const WorksetEpoch epoch =
        harness.session.snapshot().workset_epoch;

    ProgramActionDispatchResult leased =
        harness.InvokeGraph(
            CanonicalAction::InputAcquireLease,
            InputLeaseRequestGraph(false));
    ASSERT_TRUE(leased.accepted);
    ASSERT_TRUE(leased.immediate_result);
    const ProgramValue* lease_root =
        Root(leased.immediate_result->resolution.output);
    ASSERT_NE(lease_root, nullptr);
    const auto* lease =
        std::get_if<ResourceHandleValue>(
            &lease_root->payload);
    ASSERT_NE(lease, nullptr);

    const ExecutionSubmissionReceipt busy =
        harness.session.SubmitExecution(
            InteractiveResumeRequest{
                .expected_epoch = epoch});
    ASSERT_TRUE(busy.accepted) << busy.error.message;

    ProgramActionDispatchResult rejected =
        harness.InvokeGraph(
            CanonicalAction::InputPublishSequence,
            InputSequenceRequestGraph(
                *lease_root,
                std::vector<Byte>(8, 0)));
    EXPECT_FALSE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "execution_rejected");
    ASSERT_NE(harness.session.input_arbiter(), nullptr);
    EXPECT_EQ(
        harness.session.input_arbiter()
            ->snapshot()
            .binding_count,
        0u);

    (void)harness.session.CancelExecution(
        CancellationReason::ExternalRequest);
    for (int pump = 0; pump < 16; ++pump)
    {
        harness.session.PumpExecution();
        const std::vector<ExecutionEvent> events =
            harness.session.DrainExecutionEvents();
        if (std::ranges::any_of(
                events,
                [](const ExecutionEvent& event) {
                    return event.kind ==
                        ExecutionEventKind::Terminal;
                }))
        {
            break;
        }
    }
    ASSERT_TRUE(harness.Finish().accepted);
    harness.host->Shutdown();
    EXPECT_TRUE(harness.session.Shutdown().ok);
}

TEST(
    SessionProgramActionHost,
    SequenceReturnsExactPublicationUsableByPollAndContinue)
{
    HostHarness harness;
    ASSERT_TRUE(harness.Open());
    ASSERT_TRUE(harness.Prepare(
        InvocationStatePolicy::RestoreBaseline,
        "exact-input-publication").accepted);

    ProgramActionDispatchResult leased =
        harness.InvokeGraph(
            CanonicalAction::InputAcquireLease,
            InputLeaseRequestGraph(false));
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

    ProgramActionDispatchResult pulse =
        harness.InvokeGraph(
            CanonicalAction::InputPublishPulse,
            InputPulseRequestGraph(*lease, frame));
    ASSERT_TRUE(pulse.accepted)
        << pulse.diagnostic;
    AcknowledgeInputAndPumpOnce(harness);
    ASSERT_TRUE(
        harness.host->snapshot().execution_pending);
    AcknowledgeInputAndPumpOnce(harness);
    std::vector<ActorActionResult>
        pulse_completions =
            harness.host->DrainResults();
    ASSERT_EQ(pulse_completions.size(), 1u);
    CanonicalActionPayload pulse_receipt;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        pulse_completions.front().resolution.output,
        *CanonicalActionOutputSchemaIdentity(
            CanonicalAction::InputPublishPulse),
        pulse_receipt));
    const std::vector<Byte> neutral{
        0,
        0,
        128,
        128,
        128,
        128,
        0,
        0};
    ASSERT_TRUE(pulse_receipt.Bytes(
        CanonicalActionPayloadField::ResultFrame));
    EXPECT_TRUE(std::ranges::equal(
        *pulse_receipt.Bytes(
            CanonicalActionPayloadField::ResultFrame),
        neutral));

    ProgramActionDispatchResult dispatched =
        harness.InvokeGraph(
            CanonicalAction::InputPublishSequence,
            InputSequenceRequestGraph(*lease, frame));
    ASSERT_TRUE(dispatched.accepted)
        << dispatched.diagnostic;
    AcknowledgeInputAndPumpOnce(harness);
    std::vector<ActorActionResult> completions =
        harness.host->DrainResults();
    ASSERT_EQ(completions.size(), 1u);
    ASSERT_EQ(
        completions.front().resolution.status,
        ProgramActionResolutionStatus::Completed);
    const ProgramValue* publication =
        Root(completions.front().resolution.output);
    ASSERT_NE(publication, nullptr);

    CanonicalActionPayload receipt;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        completions.front().resolution.output,
        *CanonicalActionOutputSchemaIdentity(
            CanonicalAction::InputPublishSequence),
        receipt));
    EXPECT_TRUE(receipt.Unsigned(
        CanonicalActionPayloadField::Handle));
    EXPECT_TRUE(receipt.Unsigned(
        CanonicalActionPayloadField::Publication));
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

    ProgramActionDispatchResult polled =
        harness.InvokeGraph(
            CanonicalAction::InputAwaitGuestPoll,
            InputPollRequestGraph(
                *lease,
                *publication));
    ASSERT_TRUE(polled.accepted)
        << polled.diagnostic;
    ASSERT_TRUE(polled.immediate_result);
    CanonicalActionPayload poll_result;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        polled.immediate_result->resolution.output,
        *CanonicalActionOutputSchemaIdentity(
            CanonicalAction::InputAwaitGuestPoll),
        poll_result));
    EXPECT_EQ(
        poll_result.Boolean(
            CanonicalActionPayloadField::
                ResultAcknowledged),
        true);

    const TestPoint target = FirstRegisteredPcPoint();
    ProgramActionDispatchResult subscribed =
        harness.InvokeGraph(
            CanonicalAction::StopPointsSubscribeGroup,
            SubscribeRequestGraph(target));
    ASSERT_TRUE(subscribed.accepted);
    ASSERT_TRUE(subscribed.immediate_result);
    const ProgramValue* group =
        Root(subscribed.immediate_result->resolution.output);
    ASSERT_NE(group, nullptr);
    ProgramActionDispatchResult continued =
        harness.InvokeGraph(
            CanonicalAction::ExecutionContinueUntil,
            ContinueRequestGraph(
                *group,
                publication));
    ASSERT_TRUE(continued.accepted)
        << continued.diagnostic;
    EXPECT_TRUE(
        harness.session.execution_snapshot().input_bound);

    ASSERT_TRUE(harness.session.CancelExecution(
        CancellationReason::ExternalRequest).accepted);
    PumpHostExecution(harness);
    (void)harness.host->DrainResults();

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
        CanonicalActionPayloadField::ResultEpoch,
        *receipt.Unsigned(
            CanonicalActionPayloadField::ResultEpoch)));
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
                CanonicalAction::InputPublishSequence));
    ASSERT_TRUE(forged_graph.ok);
    const ProgramValue* forged_publication =
        Root(forged_graph.graph);
    ASSERT_NE(forged_publication, nullptr);
    ProgramActionDispatchResult rejected =
        harness.InvokeGraph(
            CanonicalAction::ExecutionContinueUntil,
            ContinueRequestGraph(
                *group,
                forged_publication));
    EXPECT_FALSE(rejected.accepted);
    ASSERT_TRUE(rejected.immediate_result);
    EXPECT_EQ(
        rejected.immediate_result->resolution.code,
        "input_relationship_invalid");

    ASSERT_TRUE(harness.Finish().accepted);
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
