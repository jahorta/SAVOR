#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Resources/SessionResourceLedger.h"

#include <cstdint>
#include <vector>

namespace {

using namespace savor::runtime;

constexpr WorksetEpoch kEpoch{11};
constexpr SessionId kSession{41};
constexpr ResourceOwnerId kOwner{101};
constexpr ResourceServiceId kService{201};

ResourceAcquisitionDefinition Resource(
    std::uint64_t external_id,
    ResourcePromotionPolicy promotion =
        ResourcePromotionPolicy::Forbidden,
    ResourceCleanupRequirement cleanup =
        ResourceCleanupRequirement::Mandatory)
{
    return {
        .owner = kOwner,
        .service = kService,
        .release = {
            .kind = ResourceKind::HostResource,
            .external_id = ResourceExternalId(external_id),
        },
        .promotion = promotion,
        .cleanup = cleanup,
        .diagnostic_label = "resource-" + std::to_string(external_id),
    };
}

class Dispatcher final : public IResourceReleaseDispatcher
{
public:
    ResourceReleaseResult Release(
        const ResourceReleaseRequest& request) noexcept override
    {
        requests.push_back(request);
        if (fail_external_id == request.receipt.release.external_id)
            return {ResourceReleaseStatus::Failed, "scripted failure"};
        return {ResourceReleaseStatus::Released, {}};
    }

    ResourceExternalId fail_external_id;
    std::vector<ResourceReleaseRequest> requests;
};

class SessionResourceLedgerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(ledger.Initialize(kSession, kEpoch, kOwner).success);
        root = ledger.snapshot().session_root;
    }

    void TearDown() override
    {
        if (ledger.snapshot().state != ResourceLedgerState::Closed)
            (void)ledger.Shutdown(dispatcher);
    }

    SessionResourceLedger ledger;
    ResourceScopeId root;
    Dispatcher dispatcher;
};

TEST_F(SessionResourceLedgerTest, BindsEveryResourceToOneWorksetEpoch)
{
    const auto scope = ledger.OpenSyntheticScope(root, kOwner, "item");
    ASSERT_TRUE(scope.success);
    const auto acquired = ledger.Acquire(
        scope.scope.id,
        {Resource(1), Resource(2)});
    ASSERT_TRUE(acquired.success);
    ASSERT_EQ(acquired.receipts.size(), 2u);
    EXPECT_EQ(acquired.receipts[0].acquisition_epoch, kEpoch);
    EXPECT_EQ(acquired.receipts[1].acquisition_epoch, kEpoch);
    EXPECT_EQ(ledger.snapshot().workset_epoch, kEpoch);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 2u);
}

TEST_F(SessionResourceLedgerTest, ScopeUnwindIsReverseAcquisitionOrder)
{
    const auto scope = ledger.OpenSyntheticScope(root, kOwner, "item");
    ASSERT_TRUE(scope.success);
    ASSERT_TRUE(ledger.Acquire(
        scope.scope.id,
        {Resource(1), Resource(2), Resource(3)}).success);

    const auto closed = ledger.CloseScope(scope.scope.id, dispatcher);
    EXPECT_EQ(closed.outcome, ResourceUnwindOutcome::Completed);
    ASSERT_EQ(dispatcher.requests.size(), 3u);
    EXPECT_EQ(dispatcher.requests[0].receipt.release.external_id,
              ResourceExternalId(3));
    EXPECT_EQ(dispatcher.requests[1].receipt.release.external_id,
              ResourceExternalId(2));
    EXPECT_EQ(dispatcher.requests[2].receipt.release.external_id,
              ResourceExternalId(1));
    EXPECT_EQ(ledger.snapshot().active_resource_count, 0u);
}

TEST_F(SessionResourceLedgerTest, MandatoryFailureTaintsTheWorksetLedger)
{
    const auto scope = ledger.OpenSyntheticScope(root, kOwner, "item");
    ASSERT_TRUE(scope.success);
    ASSERT_TRUE(ledger.Acquire(scope.scope.id, {Resource(1)}).success);
    dispatcher.fail_external_id = ResourceExternalId(1);

    const auto closed = ledger.CloseScope(scope.scope.id, dispatcher);
    EXPECT_EQ(closed.outcome, ResourceUnwindOutcome::CleanupFailed);
    EXPECT_EQ(closed.disposition,
              ResourceCleanupDisposition::TaintRequired);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Tainted);
}

TEST_F(SessionResourceLedgerTest, PromotionStaysWithinTheActiveWorksetTree)
{
    const auto outer = ledger.OpenSyntheticScope(root, kOwner, "outer");
    ASSERT_TRUE(outer.success);
    const auto inner = ledger.OpenSyntheticScope(
        outer.scope.id, kOwner, "inner");
    ASSERT_TRUE(inner.success);
    const auto acquired = ledger.Acquire(
        inner.scope.id,
        {Resource(1, ResourcePromotionPolicy::AnyAncestor)});
    ASSERT_TRUE(acquired.success);

    const auto promoted = ledger.Promote(
        acquired.receipts.front().id, outer.scope.id);
    ASSERT_TRUE(promoted.success);
    EXPECT_EQ(promoted.receipt.scope, outer.scope.id);
    EXPECT_EQ(promoted.receipt.acquisition_epoch, kEpoch);
}

TEST(SessionResourceLedger, RejectsZeroWorksetEpoch)
{
    SessionResourceLedger ledger;
    const auto initialized = ledger.Initialize(
        kSession, WorksetEpoch{}, kOwner);
    EXPECT_FALSE(initialized.success);
    EXPECT_EQ(initialized.error.code,
              ResourceLedgerErrorCode::InvalidArgument);
}

} // namespace
