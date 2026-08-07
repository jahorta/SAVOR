#include <gtest/gtest.h>

#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Runner/Runtime/Worksets/WorksetStager.h"
#include "Utils/Hash.h"
#include "common/ScriptedDolphinBackend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace savor::runtime;
using savor::test_support::ScriptedDolphinBackend;
using savor::test_support::ScriptedDolphinBackendControl;

class BlockingComponentProvider final
    : public IProgramBaselineComponentProvider
{
public:
    [[nodiscard]] std::string canonical_id() const override
    {
        return "test.host-only-component";
    }

    [[nodiscard]] std::uint32_t revision() const noexcept override
    {
        return 1;
    }

    ProgramBaselineComponentResult Stage(
        ProgramBaselineComponent& component) override
    {
        stage_thread = std::this_thread::get_id();
        entered.set_value();
        std::unique_lock lock(mutex);
        released.wait(lock, [&] { return allow; });
        const std::string digest = hash::sha256(
            component.immutable_bytes.data(),
            component.immutable_bytes.size());
        return digest == component.content_sha256
            ? ProgramBaselineComponentResult::Success()
            : ProgramBaselineComponentResult::Failure(
                  WorkerRejectionCode::InvalidArgument,
                  "component hash mismatch");
    }

    ProgramBaselineComponentResult Activate(
        const ProgramBaselineComponent&,
        EmulationSession&,
        bool) override
    {
        activated.store(true, std::memory_order_relaxed);
        return ProgramBaselineComponentResult::Success();
    }

    void Release()
    {
        {
            std::lock_guard lock(mutex);
            allow = true;
        }
        released.notify_all();
    }

    std::promise<void> entered;
    std::thread::id stage_thread;
    std::atomic<bool> activated{false};

private:
    std::mutex mutex;
    std::condition_variable released;
    bool allow = false;
};

class StagerNotifier final : public IWorksetStagerNotifier
{
public:
    void NotifyWorksetStagingCompletion() noexcept override
    {
        try
        {
            notified.set_value();
        }
        catch (...)
        {
        }
    }

    std::promise<void> notified;
};

class SerializedComponentProvider final
    : public IProgramBaselineComponentProvider
{
public:
    [[nodiscard]] std::string canonical_id() const override
    {
        return "test.serialized-component";
    }

    [[nodiscard]] std::uint32_t revision() const noexcept override
    {
        return 1;
    }

    ProgramBaselineComponentResult Stage(
        ProgramBaselineComponent&) override
    {
        stage_entered.set_value();
        stage_release.get_future().wait();
        return ProgramBaselineComponentResult::Success();
    }

    ProgramBaselineComponentResult Activate(
        const ProgramBaselineComponent&,
        EmulationSession&,
        bool) override
    {
        activate_entered.set_value();
        return ProgramBaselineComponentResult::Success();
    }

    std::promise<void> stage_entered;
    std::promise<void> stage_release;
    std::promise<void> activate_entered;
};

WorkerWorksetDefinition ArtifactWorkset(
    std::uint64_t workset_id,
    const std::filesystem::path& state_path)
{
    WorkerWorksetDefinition definition;
    definition.workset_id = WorkerWorksetId(workset_id);
    const auto phase = seedprobe::SeedProbeFullPhaseDefinitionV2();
    definition.phase_invocation = {
        .invocation_id = {1, 1},
        .program = phase->identity(),
    };
    definition.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = state_path,
        .state_sha256 = hash::sha256_of_file(state_path.string()),
        .compatibility = {
            "GEAE8E",
            std::string(64, 'a'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "source", .producer = "test"},
    };
    definition.baseline.lineage =
        phase->runtime_contract().baseline_lineage;
    const std::vector<std::uint8_t> bytes{'a', 'b', 'c'};
    definition.baseline.components.push_back({
        "test.host-only-component",
        1,
        "test.component/1",
        hash::sha256(bytes.data(), bytes.size()),
        ProgramBaselineComponentPolicy::ResetForEveryItem,
        bytes});
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
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);
    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.ordinal = 0;
    item.execution.execution_id = ProgramExecutionId(2);
    item.execution.attempt_id = AttemptId(3);
    item.execution.input_payload =
        seedprobe::EncodeSeedProbeExecutionInputV2(
            {savor::GCInputFrame{}});
    item.declared_terminal_bytes =
        kMinimumWorksetTerminalReservationBytes;
    item.correlation = {"job", "claim", "parent"};
    definition.items.push_back(std::move(item));
    definition.encoded_size_bytes = 256;
    return definition;
}

void WriteDtm(
    const std::filesystem::path& path,
    bool starts_from_savestate)
{
    std::vector<std::uint8_t> bytes(0x100, 0);
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
    bytes[0x00b] = 1;
    bytes[0x00c] = starts_from_savestate ? 1 : 0;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

WorkerWorksetDefinition MovieWorkset(
    std::uint64_t workset_id,
    const std::filesystem::path& dtm,
    std::optional<std::filesystem::path> startup = std::nullopt)
{
    using namespace savor::runtime::tasmovie;
    const auto phase = TasMovieValidationFullPhaseDefinitionV1();
    WorkerWorksetDefinition definition;
    definition.workset_id = WorkerWorksetId(workset_id);
    definition.phase_invocation = {
        .invocation_id = {workset_id, 1},
        .program = phase->identity(),
    };
    definition.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::ReadOnlyMovie,
        .state_path = startup.value_or(std::filesystem::path{}),
        .state_sha256 = startup
            ? hash::sha256_of_file(startup->string())
            : std::string{},
        .movie_path = dtm,
        .movie_sha256 = hash::sha256_of_file(dtm.string()),
        .compatibility = {
            "GEAE8E",
            std::string(64, 'a'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "source", .producer = "test"},
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
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);
    WorksetItemTemplate item;
    item.item_id = WorkerWorksetItemId(1);
    item.ordinal = 0;
    item.execution.execution_id = ProgramExecutionId(2);
    item.execution.attempt_id = AttemptId(3);
    item.execution.input_payload =
        EncodeTasMovieValidationExecutionInputV1({
            .operation =
                TasMovieValidationOperationV1::EstablishRootCursor,
            .dtm_path = dtm.string(),
            .startup_savestate_path = startup
                ? std::optional<std::string>(startup->string())
                : std::nullopt,
        });
    item.declared_terminal_bytes =
        kMinimumWorksetTerminalReservationBytes;
    item.correlation = {"job", "claim", "parent"};
    definition.items.push_back(std::move(item));
    definition.encoded_size_bytes = 256;
    return definition;
}

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-workset-stager-" + std::to_string(stamp));
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

TEST(WorksetStager, StagesOneSuccessorOffActorAndOnlyNotifies)
{
    TemporaryDirectory temp;
    const std::filesystem::path state = temp.path() / "input.sav";
    {
        std::ofstream output(state, std::ios::binary);
        output << "state";
    }
    auto provider = std::make_shared<BlockingComponentProvider>();
    auto components =
        std::make_shared<ProgramBaselineComponentRegistry>();
    ASSERT_TRUE(components->Register(provider).ok);
    auto notifier = std::make_shared<StagerNotifier>();
    WorksetStager stager({}, components, notifier);

    std::future<void> entered = provider->entered.get_future();
    std::future<void> notified = notifier->notified.get_future();
    const std::thread::id actor = std::this_thread::get_id();
    const WorksetStagingSubmission submitted =
        stager.Submit(ArtifactWorkset(11, state));
    ASSERT_TRUE(submitted.result.ok) << submitted.result.message;
    ASSERT_EQ(
        entered.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    EXPECT_NE(provider->stage_thread, actor);
    EXPECT_EQ(
        stager.Submit(ArtifactWorkset(12, state)).result.code,
        WorksetStagerErrorCode::CapacityExceeded);
    EXPECT_FALSE(provider->activated.load(std::memory_order_relaxed));

    provider->Release();
    ASSERT_EQ(
        notified.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    stager.Shutdown();
    std::vector<WorksetStagingCompletion> completed =
        stager.DrainResults();
    ASSERT_EQ(completed.size(), 1u);
    ASSERT_TRUE(completed[0].result.ok)
        << completed[0].result.message;
    ASSERT_TRUE(completed[0].package);
    EXPECT_EQ(
        completed[0].package->baseline_key,
        completed[0].package->definition.execution_key.baseline);
    ASSERT_TRUE(completed[0].package->artifact);
    EXPECT_EQ(
        completed[0].package->artifact->state_sha256,
        hash::sha256_of_file(state.string()));
}

TEST(WorksetStager, RejectsMismatchedArtifactHashWithoutSessionAuthority)
{
    TemporaryDirectory temp;
    const std::filesystem::path state = temp.path() / "input.sav";
    {
        std::ofstream output(state, std::ios::binary);
        output << "actual-state";
    }

    WorkerWorksetDefinition definition = ArtifactWorkset(21, state);
    definition.baseline.components.clear();
    definition.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = state,
        .state_sha256 = std::string(64, 'f'),
        .compatibility = {
            "GEAE8E",
            std::string(64, 'a'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "source", .producer = "test"},
    };
    definition.execution_key.baseline =
        ComputeProgramBaselineKey(definition.baseline);
    definition.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            definition.execution_key);

    WorksetStager stager;
    ASSERT_TRUE(stager.Submit(std::move(definition)).result.ok);
    stager.Shutdown();
    const auto completed = stager.DrainResults();
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_FALSE(completed[0].result.ok);
    EXPECT_EQ(
        completed[0].result.code,
        WorksetStagerErrorCode::ArtifactFailure);
    EXPECT_FALSE(completed[0].package);
}

TEST(
    WorksetStager,
    ValidatesReadOnlyMovieHashStartupParityNamingAndScalarIdentity)
{
    TemporaryDirectory temp;
    const std::filesystem::path root_dtm =
        temp.path() / "root.dtm";
    WriteDtm(root_dtm, false);

    WorksetStager valid_stager;
    ASSERT_TRUE(
        valid_stager.Submit(MovieWorkset(31, root_dtm)).result.ok);
    valid_stager.Shutdown();
    auto valid = valid_stager.DrainResults();
    ASSERT_EQ(valid.size(), 1u);
    ASSERT_TRUE(valid[0].result.ok) << valid[0].result.message;
    ASSERT_TRUE(valid[0].package);
    ASSERT_TRUE(valid[0].package->artifact);
    EXPECT_EQ(
        valid[0].package->artifact->movie_sha256,
        hash::sha256_of_file(root_dtm.string()));
    EXPECT_EQ(valid[0].package->artifact->state_bytes, 0u);

    const std::filesystem::path state_dtm =
        temp.path() / "from-state.dtm";
    WriteDtm(state_dtm, true);
    const std::filesystem::path wrong_startup =
        temp.path() / "wrong.sav";
    {
        std::ofstream output(wrong_startup, std::ios::binary);
        output << "startup";
    }
    WorksetStager wrong_name_stager;
    ASSERT_TRUE(wrong_name_stager
        .Submit(MovieWorkset(32, state_dtm, wrong_startup))
        .result.ok);
    wrong_name_stager.Shutdown();
    auto wrong_name = wrong_name_stager.DrainResults();
    ASSERT_EQ(wrong_name.size(), 1u);
    EXPECT_FALSE(wrong_name[0].result.ok);
    EXPECT_EQ(
        wrong_name[0].result.code,
        WorksetStagerErrorCode::ArtifactFailure);

    WorkerWorksetDefinition disagreement =
        MovieWorkset(33, root_dtm);
    disagreement.items.front().execution.input_payload =
        savor::runtime::tasmovie::
            EncodeTasMovieValidationExecutionInputV1({
                .operation = savor::runtime::tasmovie::
                    TasMovieValidationOperationV1::EstablishRootCursor,
                .dtm_path = (temp.path() / "other.dtm").string(),
            });
    WorksetStager disagreement_stager;
    ASSERT_TRUE(disagreement_stager
        .Submit(std::move(disagreement)).result.ok);
    disagreement_stager.Shutdown();
    auto disagreed = disagreement_stager.DrainResults();
    ASSERT_EQ(disagreed.size(), 1u);
    EXPECT_FALSE(disagreed[0].result.ok);
    EXPECT_EQ(
        disagreed[0].result.code,
        WorksetStagerErrorCode::InvalidArgument);

    WorkerWorksetDefinition incompatible =
        MovieWorkset(34, root_dtm);
    incompatible.baseline.artifact.compatibility.game_id =
        "OTHER0";
    incompatible.execution_key.baseline =
        ComputeProgramBaselineKey(incompatible.baseline);
    incompatible.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            incompatible.execution_key);
    WorksetStager incompatible_stager;
    ASSERT_TRUE(incompatible_stager
        .Submit(std::move(incompatible)).result.ok);
    incompatible_stager.Shutdown();
    auto incompatible_result =
        incompatible_stager.DrainResults();
    ASSERT_EQ(incompatible_result.size(), 1u);
    EXPECT_FALSE(incompatible_result[0].result.ok);
    EXPECT_EQ(
        incompatible_result[0].result.code,
        WorksetStagerErrorCode::ArtifactFailure);
}

TEST(
    WorksetStager,
    FrozenComponentRegistrySerializesHostStageAndActorActivation)
{
    auto provider =
        std::make_shared<SerializedComponentProvider>();
    auto components =
        std::make_shared<ProgramBaselineComponentRegistry>();
    ASSERT_TRUE(components->Register(provider).ok);
    components->Freeze();
    EXPECT_TRUE(components->frozen());
    EXPECT_FALSE(
        components
            ->Register(std::make_shared<
                InlineProgramBaselineComponentProvider>(
                "test.late-component"))
            .ok);

    ProgramBaselineDefinition definition;
    definition.components.push_back({
        "test.serialized-component",
        1,
        "test.serialized-component/1",
        std::string(64, 'a'),
        ProgramBaselineComponentPolicy::ResetForEveryItem,
        {1}});

    auto stage_entered = provider->stage_entered.get_future();
    auto activate_entered =
        provider->activate_entered.get_future();
    auto stage = std::async(
        std::launch::async,
        [&] { return components->Stage(definition); });
    ASSERT_EQ(
        stage_entered.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);

    auto backend =
        std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(99),
        std::make_unique<ScriptedDolphinBackend>(backend));
    auto activate = std::async(
        std::launch::async,
        [&] {
            return components->Activate(
                definition,
                session,
                true);
        });
    EXPECT_EQ(
        activate_entered.wait_for(std::chrono::milliseconds(0)),
        std::future_status::timeout);

    provider->stage_release.set_value();
    ASSERT_EQ(
        stage.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    EXPECT_TRUE(stage.get().ok);
    ASSERT_EQ(
        activate.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    EXPECT_TRUE(activate.get().ok);
    EXPECT_EQ(
        activate_entered.wait_for(std::chrono::milliseconds(0)),
        std::future_status::ready);
}

} // namespace
