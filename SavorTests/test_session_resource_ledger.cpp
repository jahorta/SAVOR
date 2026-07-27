#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Resources/SessionResourceLedger.h"

#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;

constexpr StateEpoch kEpoch{11};
constexpr SessionId kSession{41};
constexpr ResourceOwnerId kOwner{101};
constexpr ResourceServiceId kService{201};

ResourceAcquisitionDefinition Resource(
    std::uint64_t external_id,
    ResourceEpochPolicy epoch_policy =
        ResourceEpochPolicy::EndOnEpochChange,
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
        .epoch_policy = epoch_policy,
        .promotion = promotion,
        .cleanup = cleanup,
        .rebind_key =
            epoch_policy == ResourceEpochPolicy::RebindAfterRestore
            ? ResourceRebindKey(external_id + 1000)
            : ResourceRebindKey{},
        .diagnostic_label = "resource-" + std::to_string(external_id),
    };
}

class ScriptedResourceReleaseDispatcher final
    : public IResourceReleaseDispatcher
{
public:
    struct Script
    {
        ResourceExternalId external_id;
        std::vector<ResourceReleaseResult> results;
        std::size_t next = 0;
    };

    void Set(
        std::uint64_t external_id,
        std::vector<ResourceReleaseResult> results)
    {
        scripts.push_back(
            Script{ResourceExternalId(external_id), std::move(results), 0});
    }

    ResourceReleaseResult Release(
        const ResourceReleaseRequest& request) noexcept override
    {
        requests.push_back(request);
        for (Script& script : scripts)
        {
            if (script.external_id != request.receipt.release.external_id ||
                script.next >= script.results.size())
            {
                continue;
            }
            return script.results[script.next++];
        }
        return {ResourceReleaseStatus::Released, {}};
    }

    std::vector<Script> scripts;
    std::vector<ResourceReleaseRequest> requests;
};

class SessionResourceLedgerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(ledger.Initialize(kSession, kEpoch, kOwner).success);
        root = ledger.snapshot().session_root;
        ASSERT_TRUE(root);
    }

    void TearDown() override
    {
        const ResourceLedgerState state = ledger.snapshot().state;
        if (state == ResourceLedgerState::Closed ||
            state == ResourceLedgerState::Uninitialized)
        {
            return;
        }
        ASSERT_NE(state, ResourceLedgerState::Unwinding);
        ASSERT_NE(state, ResourceLedgerState::StateTransition);
        EXPECT_NE(
            ledger.Shutdown(dispatcher).outcome,
            ResourceUnwindOutcome::Rejected);
    }

    SessionResourceLedger ledger;
    ResourceScopeId root;
    ScriptedResourceReleaseDispatcher dispatcher;
};

TEST_F(
    SessionResourceLedgerTest,
    BatchAcquisitionIsAtomicAndActorAssignedIdentitiesAreMonotonic)
{
    const auto child =
        ledger.OpenSyntheticScope(root, kOwner, "action");
    ASSERT_TRUE(child.success);

    ResourceAcquisitionDefinition invalid = Resource(2);
    invalid.service = {};
    const auto rejected = ledger.Acquire(
        child.scope.id,
        {Resource(1), invalid});
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(
        rejected.error.code,
        ResourceLedgerErrorCode::InvalidArgument);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 0u);

    const auto acquired = ledger.Acquire(
        child.scope.id,
        {Resource(1), Resource(2)});
    ASSERT_TRUE(acquired.success);
    ASSERT_EQ(acquired.receipts.size(), 2u);
    EXPECT_EQ(acquired.receipts[0].id, ResourceReceiptId(1));
    EXPECT_EQ(acquired.receipts[1].id, ResourceReceiptId(2));
    EXPECT_LT(
        acquired.receipts[0].sequence,
        acquired.receipts[1].sequence);
    EXPECT_EQ(acquired.receipts[0].scope, child.scope.id);
    EXPECT_EQ(acquired.receipts[0].session, kSession);
    EXPECT_EQ(acquired.receipts[0].owner, kOwner);
    EXPECT_EQ(acquired.receipts[0].service, kService);
    EXPECT_EQ(acquired.receipts[0].acquisition_epoch, kEpoch);

    const auto duplicate = ledger.Acquire(
        child.scope.id,
        {Resource(2)});
    EXPECT_FALSE(duplicate.success);
    EXPECT_EQ(
        duplicate.error.code,
        ResourceLedgerErrorCode::ResourceAlreadyRegistered);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 2u);
}

TEST_F(
    SessionResourceLedgerTest,
    ScopeUnwindIsReverseOrderAndContinuesAfterOptionalFailure)
{
    const auto outer =
        ledger.OpenSyntheticScope(root, kOwner, "outer");
    ASSERT_TRUE(outer.success);
    const auto inner =
        ledger.OpenSyntheticScope(outer.scope.id, kOwner, "inner");
    ASSERT_TRUE(inner.success);

    ASSERT_TRUE(ledger.Acquire(outer.scope.id, {Resource(1)}).success);
    ASSERT_TRUE(ledger.Acquire(inner.scope.id, {Resource(2)}).success);
    ASSERT_TRUE(
        ledger.Acquire(
            outer.scope.id,
            {Resource(
                3,
                ResourceEpochPolicy::EndOnEpochChange,
                ResourcePromotionPolicy::Forbidden,
                ResourceCleanupRequirement::Optional)})
            .success);

    dispatcher.Set(
        3,
        {{
            ResourceReleaseStatus::Failed,
            "optional release failed",
        }});

    const auto closed = ledger.CloseScope(outer.scope.id, dispatcher);
    EXPECT_EQ(closed.outcome, ResourceUnwindOutcome::CleanupFailed);
    EXPECT_EQ(
        closed.disposition,
        ResourceCleanupDisposition::CleanWithDiagnostics);
    ASSERT_EQ(dispatcher.requests.size(), 3u);
    EXPECT_EQ(
        dispatcher.requests[0].receipt.release.external_id,
        ResourceExternalId(3));
    EXPECT_EQ(
        dispatcher.requests[1].receipt.release.external_id,
        ResourceExternalId(2));
    EXPECT_EQ(
        dispatcher.requests[2].receipt.release.external_id,
        ResourceExternalId(1));
    EXPECT_EQ(ledger.snapshot().active_resource_count, 1u);
    EXPECT_EQ(ledger.snapshot().failed_release_count, 1u);
    EXPECT_TRUE(ledger.FindScope(outer.scope.id)->open);
    EXPECT_FALSE(ledger.FindScope(inner.scope.id)->open);

    const auto retried = ledger.CloseScope(outer.scope.id, dispatcher);
    EXPECT_EQ(retried.outcome, ResourceUnwindOutcome::Completed);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 0u);
    EXPECT_FALSE(ledger.FindScope(outer.scope.id)->open);
    EXPECT_FALSE(ledger.FindScope(inner.scope.id)->open);
}

TEST_F(
    SessionResourceLedgerTest,
    MandatoryCleanupFailureTaintsAndBlocksLaterAcquisition)
{
    const auto scope =
        ledger.OpenSyntheticScope(root, kOwner, "mandatory");
    ASSERT_TRUE(scope.success);
    ASSERT_TRUE(ledger.Acquire(scope.scope.id, {Resource(1)}).success);

    dispatcher.Set(
        1,
        {{
            ResourceReleaseStatus::Failed,
            "restoration could not be proven",
        }});

    const auto closed = ledger.CloseScope(scope.scope.id, dispatcher);
    EXPECT_EQ(closed.outcome, ResourceUnwindOutcome::CleanupFailed);
    EXPECT_EQ(
        closed.disposition,
        ResourceCleanupDisposition::TaintRequired);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Tainted);

    const auto rejected = ledger.Acquire(root, {Resource(2)});
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(
        rejected.error.code,
        ResourceLedgerErrorCode::InvalidState);

    const auto shutdown = ledger.Shutdown(dispatcher);
    EXPECT_EQ(shutdown.outcome, ResourceUnwindOutcome::Completed);
    EXPECT_EQ(
        shutdown.disposition,
        ResourceCleanupDisposition::TaintRequired);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Closed);
    EXPECT_EQ(dispatcher.requests.size(), 2u);
}

TEST_F(
    SessionResourceLedgerTest,
    CleanupExecutionRequirementSuspendsUnwindWithoutReleasingEarlierResources)
{
    const auto scope =
        ledger.OpenSyntheticScope(root, kOwner, "interaction");
    ASSERT_TRUE(scope.success);
    const auto acquired =
        ledger.Acquire(scope.scope.id, {Resource(1), Resource(2)});
    ASSERT_TRUE(acquired.success);

    dispatcher.Set(
        2,
        {
            {
                ResourceReleaseStatus::CleanupExecutionRequired,
                "publish neutral and advance to a guest poll",
            },
            {ResourceReleaseStatus::Released, "neutral observed"},
        });

    const auto suspended = ledger.CloseScope(scope.scope.id, dispatcher);
    EXPECT_EQ(
        suspended.outcome,
        ResourceUnwindOutcome::CleanupExecutionRequired);
    ASSERT_TRUE(suspended.cleanup_execution_request);
    EXPECT_TRUE(suspended.cleanup_execution_request->release.cleanup_only);
    EXPECT_EQ(
        suspended.cleanup_execution_request->release.receipt.release.external_id,
        ResourceExternalId(2));
    EXPECT_EQ(ledger.snapshot().active_resource_count, 2u);
    EXPECT_TRUE(ledger.snapshot().cleanup_execution_pending);
    ASSERT_EQ(dispatcher.requests.size(), 1u);

    ScriptedResourceReleaseDispatcher wrong_dispatcher;
    EXPECT_EQ(
        ledger
            .ContinueCleanup(
                ResourceCleanupContinuationId(999),
                dispatcher)
            .error.code,
        ResourceLedgerErrorCode::InvalidArgument);
    EXPECT_EQ(
        ledger
            .ContinueCleanup(
                suspended.cleanup_execution_request->continuation,
                wrong_dispatcher)
            .error.code,
        ResourceLedgerErrorCode::InvalidArgument);
    EXPECT_EQ(dispatcher.requests.size(), 1u);
    EXPECT_TRUE(wrong_dispatcher.requests.empty());

    const auto resumed = ledger.ContinueCleanup(
        suspended.cleanup_execution_request->continuation,
        dispatcher);
    EXPECT_EQ(resumed.outcome, ResourceUnwindOutcome::Completed);
    EXPECT_EQ(
        resumed.disposition,
        ResourceCleanupDisposition::Clean);
    ASSERT_EQ(dispatcher.requests.size(), 3u);
    EXPECT_EQ(
        dispatcher.requests[1].receipt.release.external_id,
        ResourceExternalId(2));
    EXPECT_EQ(
        dispatcher.requests[2].receipt.release.external_id,
        ResourceExternalId(1));
    EXPECT_EQ(ledger.snapshot().active_resource_count, 0u);
    EXPECT_FALSE(ledger.snapshot().cleanup_execution_pending);

    const std::size_t calls = dispatcher.requests.size();
    EXPECT_EQ(
        ledger.CloseScope(scope.scope.id, dispatcher).outcome,
        ResourceUnwindOutcome::AlreadyComplete);
    EXPECT_EQ(dispatcher.requests.size(), calls);
}

TEST_F(
    SessionResourceLedgerTest,
    PromotionRequiresDescriptorPermissionAndAnEnclosingScope)
{
    const auto outer =
        ledger.OpenSyntheticScope(root, kOwner, "outer");
    ASSERT_TRUE(outer.success);
    const auto inner =
        ledger.OpenSyntheticScope(outer.scope.id, kOwner, "inner");
    ASSERT_TRUE(inner.success);

    const auto forbidden =
        ledger.Acquire(inner.scope.id, {Resource(1)});
    ASSERT_TRUE(forbidden.success);
    EXPECT_EQ(
        ledger.Promote(forbidden.receipts[0].id, outer.scope.id).error.code,
        ResourceLedgerErrorCode::PromotionForbidden);

    const auto parent_only = ledger.Acquire(
        inner.scope.id,
        {Resource(
            2,
            ResourceEpochPolicy::EndOnEpochChange,
            ResourcePromotionPolicy::ImmediateParent)});
    ASSERT_TRUE(parent_only.success);
    EXPECT_EQ(
        ledger.Promote(parent_only.receipts[0].id, root).error.code,
        ResourceLedgerErrorCode::PromotionForbidden);
    const auto promoted_parent =
        ledger.Promote(parent_only.receipts[0].id, outer.scope.id);
    EXPECT_TRUE(promoted_parent.success);
    EXPECT_TRUE(promoted_parent.changed);

    const auto any_ancestor = ledger.Acquire(
        inner.scope.id,
        {Resource(
            3,
            ResourceEpochPolicy::EndOnEpochChange,
            ResourcePromotionPolicy::AnyAncestor)});
    ASSERT_TRUE(any_ancestor.success);
    const auto promoted_root =
        ledger.Promote(any_ancestor.receipts[0].id, root);
    EXPECT_TRUE(promoted_root.success);
    EXPECT_EQ(promoted_root.receipt.scope, root);

    constexpr ResourceOwnerId kForeignOwner{102};
    const auto foreign =
        ledger.OpenSyntheticScope(root, kForeignOwner, "foreign");
    ASSERT_TRUE(foreign.success);
    ResourceAcquisitionDefinition foreign_resource = Resource(
        4,
        ResourceEpochPolicy::EndOnEpochChange,
        ResourcePromotionPolicy::AnyAncestor);
    foreign_resource.owner = kForeignOwner;
    const auto foreign_acquired =
        ledger.Acquire(foreign.scope.id, {foreign_resource});
    ASSERT_TRUE(foreign_acquired.success);
    EXPECT_EQ(
        ledger
            .Promote(foreign_acquired.receipts[0].id, root)
            .error.code,
        ResourceLedgerErrorCode::PromotionForbidden);

    const auto closed = ledger.CloseScope(inner.scope.id, dispatcher);
    EXPECT_EQ(closed.outcome, ResourceUnwindOutcome::Completed);
    ASSERT_EQ(dispatcher.requests.size(), 1u);
    EXPECT_EQ(
        dispatcher.requests[0].receipt.release.external_id,
        ResourceExternalId(1));
    EXPECT_EQ(ledger.snapshot().active_resource_count, 3u);
}

TEST_F(
    SessionResourceLedgerTest,
    StateTransitionGatesAcquisitionAndAppliesEpochPolicies)
{
    const auto acquired = ledger.Acquire(
        root,
        {
            Resource(1, ResourceEpochPolicy::EpochAgnostic),
            Resource(2, ResourceEpochPolicy::EndOnEpochChange),
            Resource(3, ResourceEpochPolicy::RebindAfterRestore),
            Resource(4, ResourceEpochPolicy::ReplacesState),
        });
    ASSERT_TRUE(acquired.success);

    ASSERT_TRUE(ledger.BeginStateTransition(kEpoch).success);
    EXPECT_EQ(
        ledger.Acquire(root, {Resource(5)}).error.code,
        ResourceLedgerErrorCode::InvalidState);
    EXPECT_EQ(
        ledger.OpenSyntheticScope(root, kOwner).error.code,
        ResourceLedgerErrorCode::InvalidState);
    EXPECT_EQ(
        ledger.Promote(acquired.receipts[0].id, root).error.code,
        ResourceLedgerErrorCode::InvalidState);

    const auto committed =
        ledger.CommitStateTransition(StateEpoch(12), dispatcher);
    EXPECT_EQ(committed.outcome, ResourceUnwindOutcome::Completed);
    EXPECT_EQ(ledger.snapshot().state_epoch, StateEpoch(12));
    EXPECT_EQ(
        ledger.snapshot().state,
        ResourceLedgerState::StateTransition);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 2u);
    ASSERT_EQ(dispatcher.requests.size(), 2u);
    EXPECT_EQ(
        dispatcher.requests[0].receipt.release.external_id,
        ResourceExternalId(3));
    EXPECT_EQ(
        dispatcher.requests[1].receipt.release.external_id,
        ResourceExternalId(2));
    EXPECT_EQ(
        dispatcher.requests[0].reason,
        ResourceReleaseReason::StateEpochChanged);
    ASSERT_EQ(committed.rebind_requests.size(), 1u);
    EXPECT_EQ(
        committed.rebind_requests[0].prior_receipt.id,
        acquired.receipts[2].id);
    EXPECT_EQ(
        committed.rebind_requests[0].stable_key,
        ResourceRebindKey(1003));
    EXPECT_EQ(
        committed.rebind_requests[0].state_epoch,
        StateEpoch(12));

    ResourceAcquisitionDefinition reused =
        Resource(3, ResourceEpochPolicy::RebindAfterRestore);
    const auto rejected_reuse =
        ledger.CompleteStateTransitionRebinds({{
            .prior_receipt = acquired.receipts[2].id,
            .replacement = reused,
        }});
    EXPECT_FALSE(rejected_reuse.success);
    EXPECT_EQ(
        rejected_reuse.error.code,
        ResourceLedgerErrorCode::InvalidArgument);
    EXPECT_EQ(
        ledger.snapshot().state,
        ResourceLedgerState::StateTransition);

    ResourceAcquisitionDefinition rebound =
        Resource(30, ResourceEpochPolicy::RebindAfterRestore);
    rebound.rebind_key = ResourceRebindKey(1003);
    const auto completed_rebind =
        ledger.CompleteStateTransitionRebinds({{
            .prior_receipt = acquired.receipts[2].id,
            .replacement = rebound,
        }});
    ASSERT_TRUE(completed_rebind.success);
    ASSERT_EQ(completed_rebind.receipts.size(), 1u);
    EXPECT_EQ(
        completed_rebind.receipts[0].rebound_from,
        acquired.receipts[2].id);
    EXPECT_EQ(
        completed_rebind.receipts[0].acquisition_epoch,
        StateEpoch(12));
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Accepting);
    EXPECT_EQ(ledger.snapshot().active_resource_count, 3u);
}

TEST_F(
    SessionResourceLedgerTest,
    OptionalStateTransitionCleanupFailureAdvancesEpochWithDiagnostics)
{
    const auto acquired = ledger.Acquire(
        root,
        {Resource(
            1,
            ResourceEpochPolicy::EndOnEpochChange,
            ResourcePromotionPolicy::Forbidden,
            ResourceCleanupRequirement::Optional)});
    ASSERT_TRUE(acquired.success);
    dispatcher.Set(
        1,
        {{
            ResourceReleaseStatus::Failed,
            "optional state-transition cleanup failed",
        }});

    ASSERT_TRUE(ledger.BeginStateTransition(kEpoch).success);
    const auto committed =
        ledger.CommitStateTransition(StateEpoch(12), dispatcher);

    EXPECT_EQ(
        committed.outcome,
        ResourceUnwindOutcome::CleanupFailed);
    EXPECT_EQ(
        committed.disposition,
        ResourceCleanupDisposition::CleanWithDiagnostics);
    const ResourceLedgerSnapshot snapshot = ledger.snapshot();
    EXPECT_EQ(snapshot.state, ResourceLedgerState::Accepting);
    EXPECT_EQ(snapshot.state_epoch, StateEpoch(12));
    EXPECT_EQ(snapshot.active_resource_count, 1u);
    EXPECT_EQ(snapshot.failed_release_count, 1u);
    EXPECT_NE(
        snapshot.diagnostic.find(
            "optional state-transition cleanup failed"),
        std::string::npos);

    const auto retried =
        ledger.Release(acquired.receipts[0].id, dispatcher);
    EXPECT_EQ(retried.outcome, ResourceUnwindOutcome::Completed);
    EXPECT_EQ(
        retried.disposition,
        ResourceCleanupDisposition::CleanWithDiagnostics);
}

TEST_F(
    SessionResourceLedgerTest,
    FailedRequiredRebindKeepsAcquisitionClosedAndRequiresTaint)
{
    ASSERT_TRUE(
        ledger.Acquire(
            root,
            {Resource(
                1,
                ResourceEpochPolicy::RebindAfterRestore)})
            .success);
    ASSERT_TRUE(ledger.BeginStateTransition(kEpoch).success);
    const auto committed =
        ledger.CommitStateTransition(StateEpoch(12), dispatcher);
    ASSERT_EQ(committed.rebind_requests.size(), 1u);
    ASSERT_EQ(
        ledger.snapshot().state,
        ResourceLedgerState::StateTransition);

    ASSERT_TRUE(
        ledger
            .FailStateTransitionRebinds(
                "router semantic binding could not be rebuilt")
            .success);
    EXPECT_EQ(
        ledger.snapshot().disposition,
        ResourceCleanupDisposition::TaintRequired);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Tainted);
    EXPECT_EQ(
        ledger.snapshot().diagnostic,
        "router semantic binding could not be rebuilt");
    EXPECT_FALSE(ledger.Acquire(root, {Resource(2)}).success);
}

TEST_F(
    SessionResourceLedgerTest,
    StateTransitionRollbackReopensTheGateWithoutChangingEpoch)
{
    ASSERT_TRUE(ledger.BeginStateTransition(kEpoch).success);
    EXPECT_FALSE(ledger.Acquire(root, {Resource(1)}).success);
    ASSERT_TRUE(ledger.RollbackStateTransition(kEpoch).success);
    EXPECT_EQ(ledger.snapshot().state_epoch, kEpoch);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Accepting);
    EXPECT_TRUE(ledger.Acquire(root, {Resource(1)}).success);
}

TEST_F(
    SessionResourceLedgerTest,
    ExplicitReleaseAndShutdownAreIdempotent)
{
    const auto acquired = ledger.Acquire(root, {Resource(1), Resource(2)});
    ASSERT_TRUE(acquired.success);

    EXPECT_EQ(
        ledger.Release(acquired.receipts[1].id, dispatcher).outcome,
        ResourceUnwindOutcome::Completed);
    const std::size_t after_release = dispatcher.requests.size();
    EXPECT_EQ(
        ledger.Release(acquired.receipts[1].id, dispatcher).outcome,
        ResourceUnwindOutcome::AlreadyComplete);
    EXPECT_EQ(dispatcher.requests.size(), after_release);

    EXPECT_EQ(
        ledger.Shutdown(dispatcher).outcome,
        ResourceUnwindOutcome::Completed);
    const std::size_t after_shutdown = dispatcher.requests.size();
    EXPECT_EQ(
        ledger.Shutdown(dispatcher).outcome,
        ResourceUnwindOutcome::AlreadyComplete);
    EXPECT_EQ(dispatcher.requests.size(), after_shutdown);
    EXPECT_EQ(ledger.snapshot().state, ResourceLedgerState::Closed);
}

TEST(SessionResourceLedgerOwnership, MutationsAreRejectedOffActorThread)
{
    SessionResourceLedger ledger;
    ASSERT_TRUE(ledger.Initialize(kSession, kEpoch, kOwner).success);
    const ResourceScopeId root = ledger.snapshot().session_root;

    ResourceScopeResult result;
    std::thread other([&] {
        result =
            ledger.OpenSyntheticScope(root, kOwner, "wrong-thread");
    });
    other.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.code, ResourceLedgerErrorCode::WrongThread);
    EXPECT_EQ(ledger.snapshot().open_scope_count, 1u);

    ScriptedResourceReleaseDispatcher dispatcher;
    EXPECT_EQ(
        ledger.Shutdown(dispatcher).outcome,
        ResourceUnwindOutcome::Completed);
}

} // namespace
