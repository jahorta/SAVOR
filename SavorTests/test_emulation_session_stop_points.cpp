#include <gtest/gtest.h>

#include "Runner/Runtime/EmulationSession.h"
#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <memory>

namespace {

using namespace savor::runtime;
using namespace savor::test_support;

class Consumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery&) override {}
};

struct Harness
{
    Harness()
        : dolphin(std::make_shared<ScriptedDolphinBackendControl>()),
          physical(std::make_shared<FakePhysicalStopBackendControl>())
    {
        session = std::make_unique<EmulationSession>(
            SessionId(41),
            MakeScriptedDolphinBackend(
                dolphin,
                std::make_unique<FakePhysicalStopBackend>(physical)));
    }

    std::shared_ptr<ScriptedDolphinBackendControl> dolphin;
    std::shared_ptr<FakePhysicalStopBackendControl> physical;
    std::unique_ptr<EmulationSession> session;
};

TEST(EmulationSessionWorksetEpoch, InfrastructureOpenHasNoActiveEpoch)
{
    Harness harness;
    const SessionOperationReceipt opened = harness.session->Open({});
    ASSERT_TRUE(opened.ok) << opened.backend.message;
    EXPECT_FALSE(opened.workset_epoch);
    EXPECT_FALSE(harness.session->snapshot().workset_epoch);
    EXPECT_EQ(harness.session->movie_service(), nullptr);
    EXPECT_EQ(harness.session->stop_points(), nullptr);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(EmulationSessionWorksetEpoch, RequiresHitTimeGuestMemoryAtStartup)
{
    Harness harness;
    harness.dolphin->hit_time_memory_available = false;
    const SessionOperationReceipt opened = harness.session->Open({});
    EXPECT_FALSE(opened.ok);
    EXPECT_EQ(opened.backend.code, BackendErrorCode::Unavailable);
    EXPECT_NE(
        opened.backend.message.find("hit-time guest-memory facet"),
        std::string::npos);
}

TEST(EmulationSessionWorksetEpoch, EpochIsStableForWorksetAndClearedAtEnd)
{
    Harness harness;
    ASSERT_TRUE(harness.session->Open({}).ok);
    const WorkerWorksetId workset(7);
    const SessionOperationReceipt initialization =
        harness.session->OpenWorksetInitialization(workset);
    ASSERT_TRUE(initialization.ok) << initialization.backend.message;
    ASSERT_TRUE(initialization.workset_epoch);
    EXPECT_EQ(
        initialization.workset_epoch,
        harness.session->snapshot().workset_epoch);
    EXPECT_FALSE(harness.session->execution_snapshot());
    EXPECT_NE(harness.session->movie_service(), nullptr);
    EXPECT_NE(harness.session->stop_points(), nullptr);
    const SessionOperationReceipt committed =
        harness.session->CommitWorksetInitialization(workset);
    ASSERT_TRUE(committed.ok) << committed.backend.message;
    ASSERT_TRUE(harness.session->execution_snapshot());

    const SessionOperationReceipt ended =
        harness.session->EndWorkset(workset);
    ASSERT_TRUE(ended.ok) << ended.backend.message;
    EXPECT_EQ(ended.workset_epoch, initialization.workset_epoch);
    EXPECT_FALSE(harness.session->snapshot().workset_epoch);
    EXPECT_EQ(harness.session->movie_service(), nullptr);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(EmulationSessionWorksetEpoch, AllocationIsMonotonicAcrossWorksets)
{
    Harness harness;
    ASSERT_TRUE(harness.session->Open({}).ok);
    const auto first = harness.session->OpenWorksetInitialization(
        WorkerWorksetId(1));
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(harness.session->CommitWorksetInitialization(
        WorkerWorksetId(1)).ok);
    ASSERT_TRUE(harness.session->EndWorkset(WorkerWorksetId(1)).ok);
    const auto second = harness.session->OpenWorksetInitialization(
        WorkerWorksetId(2));
    ASSERT_TRUE(second.ok);
    ASSERT_TRUE(harness.session->CommitWorksetInitialization(
        WorkerWorksetId(2)).ok);
    EXPECT_GT(second.workset_epoch.value(), first.workset_epoch.value());
    ASSERT_TRUE(harness.session->EndWorkset(WorkerWorksetId(2)).ok);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

TEST(EmulationSessionWorksetEpoch, PostMovieStartValidationRetainsLogicalGroups)
{
    Harness harness;
    ASSERT_TRUE(harness.session->Open({}).ok);
    ASSERT_TRUE(harness.session->OpenWorksetInitialization(
        WorkerWorksetId(3)).ok);
    ASSERT_TRUE(harness.session->CommitWorksetInitialization(
        WorkerWorksetId(3)).ok);
    StopPointRouter* router = harness.session->stop_points();
    ASSERT_NE(router, nullptr);
    Consumer consumer;
    StopSubscriptionGroupDefinition definition;
    definition.id = StopSubscriptionGroupId(1);
    definition.source = {
        StopSourceId(2), "workset.restart.test", "restart test"};
    definition.subscriptions.push_back({
        .id = StopSubscriptionId(3),
        .point = PcStopPointSpec{0x80101E48u},
        .route = PassiveStopObservation{},
        .consumer = &consumer});
    auto group = router->RegisterGroup(std::move(definition));
    ASSERT_TRUE(group.receipt.ok) << group.receipt.error.message;
    const WorksetEpoch epoch = router->workset_epoch();
    const StopDispatchGeneration dispatch = router->dispatch_generation();
    const PhysicalPlanGeneration physical = router->physical_generation();

    const StopPointLifecycleReceipt reconciled =
        router->RevalidateAfterJit();
    ASSERT_TRUE(reconciled.ok) << reconciled.error.message;
    EXPECT_EQ(router->workset_epoch(), epoch);
    EXPECT_GT(router->dispatch_generation().value(), dispatch.value());
    EXPECT_GT(router->physical_generation().value(), physical.value());
    EXPECT_EQ(router->DesiredPhysicalPlan().pcs,
              std::vector<PhysicalPcStop>{{0x80101E48u}});

    ASSERT_TRUE(group.handle.Release().ok);
    ASSERT_TRUE(harness.session->EndWorkset(WorkerWorksetId(3)).ok);
    EXPECT_TRUE(harness.session->Shutdown().ok);
}

} // namespace
