#include <gtest/gtest.h>

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

WorkerWorksetDefinition CurrentSessionWorkset(
    std::uint64_t workset_id)
{
    WorkerWorksetDefinition definition;
    definition.workset_id = WorkerWorksetId(workset_id);
    definition.baseline.state_kind =
        ProgramBaselineStateKind::CurrentSession;
    definition.baseline.current_session =
        CurrentSessionBaselineGuard{
            SessionId(7),
            StateEpoch(9),
            true};
    definition.baseline.lineage = "exact-start";
    const std::vector<std::uint8_t> bytes{'a', 'b', 'c'};
    definition.baseline.components.push_back({
        "test.host-only-component",
        1,
        "test.component/1",
        hash::sha256(bytes.data(), bytes.size()),
        ProgramBaselineComponentPolicy::ResetForEveryItem,
        bytes});
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
    item.invocation.invocation_id = InvocationId(2);
    item.invocation.attempt_id = AttemptId(3);
    item.invocation.module = definition.execution_key.module;
    item.invocation.entrypoint = "run";
    item.invocation.template_payload = {1, 2, 3};
    item.declared_active_budget = std::chrono::seconds(2);
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
        stager.Submit(CurrentSessionWorkset(11));
    ASSERT_TRUE(submitted.result.ok) << submitted.result.message;
    ASSERT_EQ(
        entered.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    EXPECT_NE(provider->stage_thread, actor);
    EXPECT_EQ(
        stager.Submit(CurrentSessionWorkset(12)).result.code,
        WorksetStagerErrorCode::CapacityExceeded);
    EXPECT_FALSE(provider->activated.load(std::memory_order_relaxed));

    provider->Release();
    ASSERT_EQ(
        notified.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);
    stager.Shutdown();
    std::vector<WorksetStagingCompletion> completed =
        stager.DrainCompletions();
    ASSERT_EQ(completed.size(), 1u);
    ASSERT_TRUE(completed[0].result.ok)
        << completed[0].result.message;
    ASSERT_TRUE(completed[0].package);
    EXPECT_EQ(
        completed[0].package->baseline_key,
        completed[0].package->definition.execution_key.baseline);
    EXPECT_FALSE(completed[0].package->artifact);
}

TEST(WorksetStager, RejectsMismatchedArtifactHashWithoutSessionAuthority)
{
    TemporaryDirectory temp;
    const std::filesystem::path state = temp.path() / "input.sav";
    {
        std::ofstream output(state, std::ios::binary);
        output << "actual-state";
    }

    WorkerWorksetDefinition definition = CurrentSessionWorkset(21);
    definition.baseline.components.clear();
    definition.baseline.state_kind =
        ProgramBaselineStateKind::Artifact;
    definition.baseline.current_session.reset();
    definition.baseline.artifact = ProgramBaselineArtifact{
        .state_path = state,
        .state_sha256 = std::string(64, 'f'),
        .movie_mode = ExternalMovieImportMode::NoMovie,
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
    const auto completed = stager.DrainCompletions();
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_FALSE(completed[0].result.ok);
    EXPECT_EQ(
        completed[0].result.code,
        WorksetStagerErrorCode::ArtifactFailure);
    EXPECT_FALSE(completed[0].package);
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
