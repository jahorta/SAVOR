#include "SessionResourceLedger.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] ResourceLedgerError Error(
    ResourceLedgerErrorCode code,
    std::string message)
{
    return ResourceLedgerError{code, std::move(message)};
}

[[nodiscard]] ResourceOperationResult Success()
{
    return ResourceOperationResult{true, {}};
}

[[nodiscard]] bool IsLive(ResourceRecordStatus status) noexcept
{
    return status == ResourceRecordStatus::Active ||
        status == ResourceRecordStatus::ReleaseFailed;
}

[[nodiscard]] ResourceCleanupDisposition MergeDiagnosticDisposition(
    ResourceCleanupDisposition disposition) noexcept
{
    return disposition == ResourceCleanupDisposition::Clean
        ? ResourceCleanupDisposition::CleanWithDiagnostics
        : disposition;
}

} // namespace

SessionResourceLedger::SessionResourceLedger(
    std::thread::id owner_thread) noexcept
    : owner_thread_(owner_thread)
{
}

ResourceOperationResult SessionResourceLedger::Initialize(
    SessionId session,
    StateEpoch state_epoch,
    ResourceOwnerId session_owner)
{
    if (!OnOwnerThread())
    {
        return FailedOperation(
            ResourceLedgerErrorCode::WrongThread,
            "SessionResourceLedger initialized outside its owner thread");
    }
    if (state_ != ResourceLedgerState::Uninitialized)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::InvalidState,
            "SessionResourceLedger is already initialized");
    }
    if (!session || !state_epoch || !session_owner)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::InvalidArgument,
            "SessionResourceLedger requires nonzero session, epoch, and owner identities");
    }

    try
    {
        ResourceScopeReceipt root;
        root.session = session;
        root.id = ResourceScopeId(next_scope_id_);
        root.owner = session_owner;
        root.kind = ResourceScopeKind::SessionRoot;
        root.open = true;
        root.diagnostic_label = "session";

        std::vector<ScopeRecord> candidate;
        candidate.push_back(ScopeRecord{root});

        scopes_.swap(candidate);
        session_root_ = root.id;
        session_id_ = session;
        ++next_scope_id_;
        state_epoch_ = state_epoch;
        disposition_ = ResourceCleanupDisposition::Clean;
        state_ = ResourceLedgerState::Accepting;
        return Success();
    }
    catch (...)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::InternalFailure,
            "SessionResourceLedger could not allocate its session root");
    }
}

ResourceScopeResult SessionResourceLedger::OpenSyntheticScope(
    ResourceScopeId parent,
    ResourceOwnerId owner,
    std::string diagnostic_label)
{
    if (!OnOwnerThread())
    {
        return ResourceScopeResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::WrongThread,
                "Resource scope opened outside the ledger owner thread")};
    }
    if (const ResourceLedgerError state_error = ValidateAccepting())
        return ResourceScopeResult{false, {}, state_error};
    if (const ResourceLedgerError scope_error =
            ValidateScopeForAcquisition(parent))
    {
        return ResourceScopeResult{false, {}, scope_error};
    }
    if (!owner)
    {
        return ResourceScopeResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InvalidArgument,
                "Resource scope owner must be nonzero")};
    }
    if (next_scope_id_ == std::numeric_limits<std::uint64_t>::max())
    {
        return ResourceScopeResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::ExhaustedIdentity,
                "Resource scope identity space is exhausted")};
    }

    ResourceScopeReceipt receipt;
    receipt.session = session_id_;
    receipt.id = ResourceScopeId(next_scope_id_);
    receipt.parent = parent;
    receipt.owner = owner;
    receipt.kind = ResourceScopeKind::Synthetic;
    receipt.open = true;
    receipt.diagnostic_label = std::move(diagnostic_label);

    try
    {
        auto candidate = scopes_;
        candidate.push_back(ScopeRecord{receipt});
        scopes_.swap(candidate);
        ++next_scope_id_;
        return ResourceScopeResult{true, std::move(receipt), {}};
    }
    catch (...)
    {
        return ResourceScopeResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InternalFailure,
                "Resource scope registration could not be committed")};
    }
}

ResourceAcquisitionResult SessionResourceLedger::Acquire(
    ResourceScopeId scope,
    const std::vector<ResourceAcquisitionDefinition>& definitions)
{
    if (!OnOwnerThread())
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::WrongThread,
                "Resources acquired outside the ledger owner thread")};
    }
    if (const ResourceLedgerError state_error = ValidateAccepting())
        return ResourceAcquisitionResult{false, {}, state_error};
    if (const ResourceLedgerError scope_error =
            ValidateScopeForAcquisition(scope))
    {
        return ResourceAcquisitionResult{false, {}, scope_error};
    }
    if (definitions.empty())
        return ResourceAcquisitionResult{true, {}, {}};

    const auto owning_scope = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == scope;
        });

    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const auto count = static_cast<std::uint64_t>(definitions.size());
    if (count > maximum - next_resource_id_ ||
        count > maximum - next_sequence_)
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::ExhaustedIdentity,
                "Resource receipt identity space is exhausted")};
    }

    for (std::size_t index = 0; index < definitions.size(); ++index)
    {
        const ResourceAcquisitionDefinition& definition = definitions[index];
        if (!definition.owner || !definition.service ||
            !definition.release.external_id)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "Resource owner, service, and external identity must be nonzero")};
        }
        if (owning_scope == scopes_.end() ||
            definition.owner != owning_scope->receipt.owner)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "Resource owner does not match the owning scope")};
        }
        if (definition.epoch_policy ==
                ResourceEpochPolicy::RebindAfterRestore &&
            !definition.rebind_key)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "RebindAfterRestore requires a stable rebind key")};
        }

        const auto same_identity =
            [&definition](const ResourceReceipt& existing) {
                return IsLive(existing.status) &&
                    existing.service == definition.service &&
                    existing.release == definition.release;
            };
        if (std::any_of(resources_.begin(), resources_.end(), same_identity))
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::ResourceAlreadyRegistered,
                    "The service resource is already registered")};
        }

        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (definitions[earlier].service == definition.service &&
                definitions[earlier].release == definition.release)
            {
                return ResourceAcquisitionResult{
                    false,
                    {},
                    Error(
                        ResourceLedgerErrorCode::ResourceAlreadyRegistered,
                        "The acquisition batch contains a duplicate service resource")};
            }
        }
    }

    try
    {
        auto candidate = resources_;
        std::vector<ResourceReceipt> receipts;
        candidate.reserve(candidate.size() + definitions.size());
        receipts.reserve(definitions.size());

        std::uint64_t resource_id = next_resource_id_;
        std::uint64_t sequence = next_sequence_;
        for (const ResourceAcquisitionDefinition& definition : definitions)
        {
            ResourceReceipt receipt;
            receipt.session = session_id_;
            receipt.id = ResourceReceiptId(resource_id++);
            receipt.sequence = ResourceAcquisitionSequence(sequence++);
            receipt.owner = definition.owner;
            receipt.service = definition.service;
            receipt.scope = scope;
            receipt.release = definition.release;
            receipt.acquisition_epoch = state_epoch_;
            receipt.epoch_policy = definition.epoch_policy;
            receipt.promotion = definition.promotion;
            receipt.cleanup = definition.cleanup;
            receipt.rebind_key = definition.rebind_key;
            receipt.diagnostic_label = definition.diagnostic_label;

            candidate.push_back(receipt);
            receipts.push_back(std::move(receipt));
        }

        resources_.swap(candidate);
        next_resource_id_ = resource_id;
        next_sequence_ = sequence;
        return ResourceAcquisitionResult{true, std::move(receipts), {}};
    }
    catch (...)
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InternalFailure,
                "Resource acquisition batch could not be committed")};
    }
}

ResourcePromotionResult SessionResourceLedger::Promote(
    ResourceReceiptId resource,
    ResourceScopeId destination_scope)
{
    if (!OnOwnerThread())
    {
        return ResourcePromotionResult{
            false,
            false,
            {},
            Error(
                ResourceLedgerErrorCode::WrongThread,
                "Resource promoted outside the ledger owner thread")};
    }
    if (const ResourceLedgerError state_error = ValidateAccepting())
        return ResourcePromotionResult{false, false, {}, state_error};
    if (const ResourceLedgerError scope_error =
            ValidateScopeForAcquisition(destination_scope))
    {
        return ResourcePromotionResult{false, false, {}, scope_error};
    }

    const auto found = std::find_if(
        resources_.begin(),
        resources_.end(),
        [resource](const ResourceReceipt& candidate) {
            return candidate.id == resource;
        });
    if (found == resources_.end())
    {
        return ResourcePromotionResult{
            false,
            false,
            {},
            Error(
                ResourceLedgerErrorCode::ResourceNotFound,
                "Resource receipt is not registered")};
    }
    if (!IsLive(found->status))
    {
        return ResourcePromotionResult{
            false,
            false,
            *found,
            Error(
                ResourceLedgerErrorCode::InvalidState,
                "Only an active resource may be promoted")};
    }
    const auto destination = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [destination_scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == destination_scope;
        });
    if (destination == scopes_.end() ||
        destination->receipt.owner != found->owner)
    {
        return ResourcePromotionResult{
            false,
            false,
            *found,
            Error(
                ResourceLedgerErrorCode::PromotionForbidden,
                "Resource promotion cannot cross scope ownership")};
    }
    if (found->scope == destination_scope)
        return ResourcePromotionResult{true, false, *found, {}};
    if (!IsAncestor(destination_scope, found->scope))
    {
        return ResourcePromotionResult{
            false,
            false,
            *found,
            Error(
                ResourceLedgerErrorCode::DestinationNotAncestor,
                "Resource promotion destination is not an enclosing scope")};
    }

    switch (found->promotion)
    {
    case ResourcePromotionPolicy::Forbidden:
        return ResourcePromotionResult{
            false,
            false,
            *found,
            Error(
                ResourceLedgerErrorCode::PromotionForbidden,
                "The resource descriptor forbids promotion")};
    case ResourcePromotionPolicy::ImmediateParent:
        if (ParentOf(found->scope) != destination_scope)
        {
            return ResourcePromotionResult{
                false,
                false,
                *found,
                Error(
                    ResourceLedgerErrorCode::PromotionForbidden,
                    "The resource may be promoted only to its immediate parent")};
        }
        break;
    case ResourcePromotionPolicy::AnyAncestor:
        break;
    }

    found->scope = destination_scope;
    return ResourcePromotionResult{true, true, *found, {}};
}

ResourceUnwindResult SessionResourceLedger::Release(
    ResourceReceiptId resource,
    IResourceReleaseDispatcher& dispatcher)
{
    if (!OnOwnerThread())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::WrongThread,
            "Resource released outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::Accepting &&
        state_ != ResourceLedgerState::Tainted)
    {
        return RejectedUnwind(
            state_ == ResourceLedgerState::Unwinding
                ? ResourceLedgerErrorCode::CleanupInProgress
                : ResourceLedgerErrorCode::InvalidState,
            "The ledger cannot begin an explicit release in its current state");
    }

    const auto found = std::find_if(
        resources_.begin(),
        resources_.end(),
        [resource](const ResourceReceipt& candidate) {
            return candidate.id == resource;
        });
    if (found == resources_.end())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::ResourceNotFound,
            "Resource receipt is not registered");
    }
    if (!IsLive(found->status))
    {
        ResourceUnwindResult result;
        result.outcome = ResourceUnwindOutcome::AlreadyComplete;
        result.disposition = disposition_;
        return result;
    }

    const ResourceLedgerState completion_state = state_;
    return StartUnwind(
        {resource},
        {},
        ResourceReleaseReason::Explicit,
        completion_state,
        false,
        dispatcher);
}

ResourceUnwindResult SessionResourceLedger::CloseScope(
    ResourceScopeId scope,
    IResourceReleaseDispatcher& dispatcher)
{
    if (!OnOwnerThread())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::WrongThread,
            "Resource scope closed outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::Accepting &&
        state_ != ResourceLedgerState::Tainted)
    {
        return RejectedUnwind(
            state_ == ResourceLedgerState::Unwinding
                ? ResourceLedgerErrorCode::CleanupInProgress
                : ResourceLedgerErrorCode::InvalidState,
            "The ledger cannot close a scope in its current state");
    }
    if (scope == session_root_)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidArgument,
            "The session root is closed only by Shutdown");
    }

    const auto scope_it = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == scope;
        });
    if (scope_it == scopes_.end())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::ScopeNotFound,
            "Resource scope is not registered");
    }
    if (!scope_it->receipt.open)
    {
        ResourceUnwindResult result;
        result.outcome = ResourceUnwindOutcome::AlreadyComplete;
        result.disposition = disposition_;
        return result;
    }

    std::vector<ResourceReceiptId> resources;
    for (const ResourceReceipt& receipt : resources_)
    {
        if (IsLive(receipt.status) &&
            IsInScopeSubtree(receipt.scope, scope))
        {
            resources.push_back(receipt.id);
        }
    }
    std::sort(
        resources.begin(),
        resources.end(),
        [this](ResourceReceiptId lhs, ResourceReceiptId rhs) {
            const auto left = FindResource(lhs);
            const auto right = FindResource(rhs);
            return left && right && left->sequence > right->sequence;
        });

    std::vector<ResourceScopeId> scopes_to_close;
    for (const ScopeRecord& candidate : scopes_)
    {
        if (candidate.receipt.open &&
            IsInScopeSubtree(candidate.receipt.id, scope))
        {
            scopes_to_close.push_back(candidate.receipt.id);
        }
    }
    std::sort(
        scopes_to_close.begin(),
        scopes_to_close.end(),
        [this](ResourceScopeId lhs, ResourceScopeId rhs) {
            auto depth = [this](ResourceScopeId candidate) {
                std::size_t value = 0;
                while (candidate && candidate != session_root_)
                {
                    candidate = ParentOf(candidate);
                    ++value;
                }
                return value;
            };
            return depth(lhs) > depth(rhs);
        });

    return StartUnwind(
        std::move(resources),
        std::move(scopes_to_close),
        ResourceReleaseReason::ScopeExit,
        state_,
        false,
        dispatcher);
}

ResourceOperationResult SessionResourceLedger::BeginStateTransition(
    StateEpoch expected_epoch)
{
    if (!OnOwnerThread())
    {
        return FailedOperation(
            ResourceLedgerErrorCode::WrongThread,
            "State transition began outside the ledger owner thread");
    }
    if (const ResourceLedgerError state_error = ValidateAccepting())
        return ResourceOperationResult{false, state_error};
    if (!expected_epoch || expected_epoch != state_epoch_)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::StaleEpoch,
            "State transition expected a different StateEpoch");
    }

    transition_origin_epoch_ = state_epoch_;
    state_ = ResourceLedgerState::StateTransition;
    return Success();
}

ResourceUnwindResult SessionResourceLedger::CommitStateTransition(
    StateEpoch new_epoch,
    IResourceReleaseDispatcher& dispatcher)
{
    if (!OnOwnerThread())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::WrongThread,
            "State transition committed outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::StateTransition)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidState,
            "No state transition is active");
    }
    if (!new_epoch || new_epoch.value() <= state_epoch_.value())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::StaleEpoch,
            "Committed StateEpoch must advance monotonically");
    }

    // The backend state replacement is already authoritative when this method
    // is called. The ledger therefore advances its epoch before closing old
    // guest-derived receipts. A cleanup failure cannot roll the guest back.
    state_epoch_ = new_epoch;

    std::vector<ResourceReceiptId> resources;
    for (const ResourceReceipt& receipt : resources_)
    {
        if (!IsLive(receipt.status))
            continue;
        if (receipt.epoch_policy == ResourceEpochPolicy::EndOnEpochChange ||
            receipt.epoch_policy == ResourceEpochPolicy::RebindAfterRestore)
        {
            resources.push_back(receipt.id);
        }
    }
    std::sort(
        resources.begin(),
        resources.end(),
        [this](ResourceReceiptId lhs, ResourceReceiptId rhs) {
            const auto left = FindResource(lhs);
            const auto right = FindResource(rhs);
            return left && right && left->sequence > right->sequence;
        });

    return StartUnwind(
        std::move(resources),
        {},
        ResourceReleaseReason::StateEpochChanged,
        ResourceLedgerState::Accepting,
        false,
        dispatcher);
}

ResourceOperationResult SessionResourceLedger::RollbackStateTransition(
    StateEpoch expected_epoch)
{
    if (!OnOwnerThread())
    {
        return FailedOperation(
            ResourceLedgerErrorCode::WrongThread,
            "State transition rolled back outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::StateTransition)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::InvalidState,
            "No state transition is active");
    }
    if (!expected_epoch || expected_epoch != transition_origin_epoch_ ||
        expected_epoch != state_epoch_)
    {
        return FailedOperation(
            ResourceLedgerErrorCode::StaleEpoch,
            "State transition rollback expected a different StateEpoch");
    }

    transition_origin_epoch_ = {};
    state_ = ResourceLedgerState::Accepting;
    return Success();
}

ResourceAcquisitionResult
SessionResourceLedger::CompleteStateTransitionRebinds(
    const std::vector<ResourceRebindCompletion>& completions)
{
    if (!OnOwnerThread())
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::WrongThread,
                "State-transition rebinds completed outside the ledger owner thread")};
    }
    if (state_ != ResourceLedgerState::StateTransition ||
        pending_rebinds_.empty())
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InvalidState,
                "No state-transition rebind set is awaiting completion")};
    }
    if (completions.size() != pending_rebinds_.size())
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InvalidArgument,
                "Every requested state-transition rebind must complete atomically")};
    }

    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const auto count = static_cast<std::uint64_t>(completions.size());
    if (count > maximum - next_resource_id_ ||
        count > maximum - next_sequence_)
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::ExhaustedIdentity,
                "Resource receipt identity space is exhausted")};
    }

    for (std::size_t index = 0; index < completions.size(); ++index)
    {
        const ResourceRebindCompletion& completion = completions[index];
        const auto request = std::find_if(
            pending_rebinds_.begin(),
            pending_rebinds_.end(),
            [&completion](const ResourceRebindRequest& candidate) {
                return candidate.prior_receipt.id ==
                    completion.prior_receipt;
            });
        if (request == pending_rebinds_.end())
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "A rebind completion does not match a requested prior receipt")};
        }
        if (std::count_if(
                completions.begin(),
                completions.end(),
                [&completion](const ResourceRebindCompletion& candidate) {
                    return candidate.prior_receipt ==
                        completion.prior_receipt;
                }) != 1)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "A prior receipt appears more than once in the rebind batch")};
        }

        const ResourceAcquisitionDefinition& replacement =
            completion.replacement;
        if (replacement.owner != request->owner ||
            replacement.service != request->service ||
            replacement.release.kind != request->kind ||
            replacement.epoch_policy !=
                ResourceEpochPolicy::RebindAfterRestore ||
            replacement.rebind_key != request->stable_key ||
            replacement.promotion !=
                request->prior_receipt.promotion ||
            replacement.cleanup != request->prior_receipt.cleanup ||
            !replacement.release.external_id ||
            replacement.release.external_id ==
                request->prior_receipt.release.external_id)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "A rebound resource changed immutable ownership, policy, or reused its old opaque handle")};
        }

        const ResourceLedgerError scope_error =
            ValidateScopeForAcquisition(request->scope);
        if (scope_error)
            return ResourceAcquisitionResult{false, {}, scope_error};
        const auto scope = std::find_if(
            scopes_.begin(),
            scopes_.end(),
            [&request](const ScopeRecord& candidate) {
                return candidate.receipt.id == request->scope;
            });
        if (scope == scopes_.end() ||
            scope->receipt.owner != replacement.owner)
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::InvalidArgument,
                    "A rebound resource does not match its retained scope owner")};
        }

        const auto duplicate =
            [&replacement](const ResourceReceipt& existing) {
                return IsLive(existing.status) &&
                    existing.service == replacement.service &&
                    existing.release == replacement.release;
            };
        if (std::any_of(resources_.begin(), resources_.end(), duplicate))
        {
            return ResourceAcquisitionResult{
                false,
                {},
                Error(
                    ResourceLedgerErrorCode::ResourceAlreadyRegistered,
                    "A rebound service resource is already active")};
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (completions[earlier].replacement.service ==
                    replacement.service &&
                completions[earlier].replacement.release ==
                    replacement.release)
            {
                return ResourceAcquisitionResult{
                    false,
                    {},
                    Error(
                        ResourceLedgerErrorCode::ResourceAlreadyRegistered,
                        "The rebind batch contains a duplicate service resource")};
            }
        }
    }

    try
    {
        auto candidate = resources_;
        std::vector<ResourceReceipt> receipts;
        candidate.reserve(candidate.size() + completions.size());
        receipts.reserve(completions.size());
        std::uint64_t resource_id = next_resource_id_;
        std::uint64_t sequence = next_sequence_;

        for (const ResourceRebindCompletion& completion : completions)
        {
            const auto request = std::find_if(
                pending_rebinds_.begin(),
                pending_rebinds_.end(),
                [&completion](const ResourceRebindRequest& candidate_request) {
                    return candidate_request.prior_receipt.id ==
                        completion.prior_receipt;
                });
            const ResourceAcquisitionDefinition& replacement =
                completion.replacement;

            ResourceReceipt receipt;
            receipt.session = session_id_;
            receipt.id = ResourceReceiptId(resource_id++);
            receipt.sequence = ResourceAcquisitionSequence(sequence++);
            receipt.owner = replacement.owner;
            receipt.service = replacement.service;
            receipt.scope = request->scope;
            receipt.release = replacement.release;
            receipt.acquisition_epoch = state_epoch_;
            receipt.epoch_policy = replacement.epoch_policy;
            receipt.promotion = replacement.promotion;
            receipt.cleanup = replacement.cleanup;
            receipt.rebind_key = replacement.rebind_key;
            receipt.rebound_from = completion.prior_receipt;
            receipt.diagnostic_label = replacement.diagnostic_label;
            candidate.push_back(receipt);
            receipts.push_back(std::move(receipt));
        }

        resources_.swap(candidate);
        next_resource_id_ = resource_id;
        next_sequence_ = sequence;
        pending_rebinds_.clear();
        transition_origin_epoch_ = {};
        state_ = ResourceLedgerState::Accepting;
        return ResourceAcquisitionResult{true, std::move(receipts), {}};
    }
    catch (...)
    {
        return ResourceAcquisitionResult{
            false,
            {},
            Error(
                ResourceLedgerErrorCode::InternalFailure,
                "State-transition rebind batch could not be committed")};
    }
}

ResourceOperationResult SessionResourceLedger::FailStateTransitionRebinds(
    std::string diagnostic)
{
    if (!OnOwnerThread())
    {
        return FailedOperation(
            ResourceLedgerErrorCode::WrongThread,
            "State-transition rebind failure reported outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::StateTransition ||
        pending_rebinds_.empty())
    {
        return FailedOperation(
            ResourceLedgerErrorCode::InvalidState,
            "No state-transition rebind set is awaiting completion");
    }

    diagnostic_ = diagnostic.empty()
        ? "A mandatory state-transition resource could not be rebound"
        : std::move(diagnostic);
    pending_rebinds_.clear();
    transition_origin_epoch_ = {};
    disposition_ = ResourceCleanupDisposition::TaintRequired;
    state_ = ResourceLedgerState::Tainted;
    return Success();
}

ResourceUnwindResult SessionResourceLedger::ContinueCleanup(
    ResourceCleanupContinuationId continuation,
    IResourceReleaseDispatcher& dispatcher)
{
    if (!OnOwnerThread())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::WrongThread,
            "Resource cleanup continued outside the ledger owner thread");
    }
    if (state_ != ResourceLedgerState::Unwinding || !pending_unwind_)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidState,
            "No resource cleanup is awaiting continuation");
    }
    if (!continuation ||
        continuation != pending_unwind_->continuation)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidArgument,
            "Cleanup continuation does not match the suspended unwind");
    }
    if (bound_dispatcher_ != &dispatcher)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidArgument,
            "Resource cleanup dispatcher does not match the ledger-bound dispatcher");
    }

    return PumpUnwind(dispatcher);
}

ResourceUnwindResult SessionResourceLedger::Shutdown(
    IResourceReleaseDispatcher& dispatcher)
{
    if (!OnOwnerThread())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::WrongThread,
            "Resource ledger shut down outside its owner thread");
    }
    if (state_ == ResourceLedgerState::Closed)
    {
        ResourceUnwindResult result;
        result.outcome = ResourceUnwindOutcome::AlreadyComplete;
        result.disposition = disposition_;
        return result;
    }
    if (state_ == ResourceLedgerState::Uninitialized)
    {
        state_ = ResourceLedgerState::Closed;
        ResourceUnwindResult result;
        result.outcome = ResourceUnwindOutcome::Completed;
        result.disposition = disposition_;
        return result;
    }
    if (state_ == ResourceLedgerState::Unwinding)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::CleanupInProgress,
            "Resource cleanup must finish before Shutdown can begin");
    }
    if (state_ == ResourceLedgerState::StateTransition)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidState,
            "An active state transition must commit or roll back before Shutdown");
    }

    std::vector<ResourceReceiptId> resources;
    for (const ResourceReceipt& receipt : resources_)
    {
        if (IsLive(receipt.status))
            resources.push_back(receipt.id);
    }
    std::sort(
        resources.begin(),
        resources.end(),
        [this](ResourceReceiptId lhs, ResourceReceiptId rhs) {
            const auto left = FindResource(lhs);
            const auto right = FindResource(rhs);
            return left && right && left->sequence > right->sequence;
        });

    std::vector<ResourceScopeId> scopes_to_close;
    for (const ScopeRecord& scope : scopes_)
    {
        if (scope.receipt.open)
            scopes_to_close.push_back(scope.receipt.id);
    }

    return StartUnwind(
        std::move(resources),
        std::move(scopes_to_close),
        ResourceReleaseReason::Shutdown,
        ResourceLedgerState::Closed,
        true,
        dispatcher);
}

ResourceLedgerSnapshot SessionResourceLedger::snapshot() const noexcept
{
    ResourceLedgerSnapshot result;
    result.state = state_;
    result.disposition = disposition_;
    result.session = session_id_;
    result.state_epoch = state_epoch_;
    result.session_root = session_root_;
    result.open_scope_count = static_cast<std::size_t>(std::count_if(
        scopes_.begin(),
        scopes_.end(),
        [](const ScopeRecord& scope) { return scope.receipt.open; }));
    result.active_resource_count = static_cast<std::size_t>(std::count_if(
        resources_.begin(),
        resources_.end(),
        [](const ResourceReceipt& resource) {
            return IsLive(resource.status);
        }));
    result.failed_release_count = static_cast<std::size_t>(std::count_if(
        resources_.begin(),
        resources_.end(),
        [](const ResourceReceipt& resource) {
            return resource.status == ResourceRecordStatus::ReleaseFailed;
        }));
    result.cleanup_execution_pending =
        pending_unwind_ &&
        !pending_unwind_->steps.empty() &&
        pending_unwind_->steps.back().result.status ==
            ResourceReleaseStatus::CleanupExecutionRequired;
    result.diagnostic = diagnostic_;
    return result;
}

std::optional<ResourceReceipt> SessionResourceLedger::FindResource(
    ResourceReceiptId resource) const
{
    const auto found = std::find_if(
        resources_.begin(),
        resources_.end(),
        [resource](const ResourceReceipt& candidate) {
            return candidate.id == resource;
        });
    if (found == resources_.end())
        return std::nullopt;
    return *found;
}

std::optional<ResourceScopeReceipt> SessionResourceLedger::FindScope(
    ResourceScopeId scope) const
{
    const auto found = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == scope;
        });
    if (found == scopes_.end())
        return std::nullopt;
    return found->receipt;
}

bool SessionResourceLedger::OnOwnerThread() const noexcept
{
    return std::this_thread::get_id() == owner_thread_;
}

ResourceLedgerError SessionResourceLedger::ValidateAccepting() const
{
    if (state_ == ResourceLedgerState::Accepting)
        return {};
    if (state_ == ResourceLedgerState::Unwinding)
    {
        return Error(
            ResourceLedgerErrorCode::CleanupInProgress,
            "Resource cleanup is in progress");
    }
    return Error(
        ResourceLedgerErrorCode::InvalidState,
        "The resource ledger is not accepting acquisitions");
}

ResourceLedgerError SessionResourceLedger::ValidateScopeForAcquisition(
    ResourceScopeId scope) const
{
    if (!scope)
    {
        return Error(
            ResourceLedgerErrorCode::InvalidArgument,
            "Resource scope identity must be nonzero");
    }
    const auto found = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == scope;
        });
    if (found == scopes_.end())
    {
        return Error(
            ResourceLedgerErrorCode::ScopeNotFound,
            "Resource scope is not registered");
    }
    if (!found->receipt.open)
    {
        return Error(
            ResourceLedgerErrorCode::ScopeClosed,
            "Resource scope is already closed");
    }
    return {};
}

bool SessionResourceLedger::IsAncestor(
    ResourceScopeId possible_ancestor,
    ResourceScopeId scope) const
{
    if (!possible_ancestor || !scope || possible_ancestor == scope)
        return false;

    ResourceScopeId cursor = ParentOf(scope);
    while (cursor)
    {
        if (cursor == possible_ancestor)
            return true;
        cursor = ParentOf(cursor);
    }
    return false;
}

bool SessionResourceLedger::IsInScopeSubtree(
    ResourceScopeId candidate,
    ResourceScopeId subtree_root) const
{
    if (candidate == subtree_root)
        return true;
    return IsAncestor(subtree_root, candidate);
}

ResourceScopeId SessionResourceLedger::ParentOf(ResourceScopeId scope) const
{
    const auto found = std::find_if(
        scopes_.begin(),
        scopes_.end(),
        [scope](const ScopeRecord& candidate) {
            return candidate.receipt.id == scope;
        });
    return found == scopes_.end() ? ResourceScopeId{} : found->receipt.parent;
}

ResourceUnwindResult SessionResourceLedger::StartUnwind(
    std::vector<ResourceReceiptId> resources,
    std::vector<ResourceScopeId> scopes_to_close,
    ResourceReleaseReason reason,
    ResourceLedgerState completion_state,
    bool shutdown,
    IResourceReleaseDispatcher& dispatcher)
{
    if (pending_unwind_)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::CleanupInProgress,
            "Another resource unwind is already active");
    }
    if (bound_dispatcher_ && bound_dispatcher_ != &dispatcher)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidArgument,
            "Resource cleanup dispatcher does not match the ledger-bound dispatcher");
    }
    if (next_continuation_id_ ==
        std::numeric_limits<std::uint64_t>::max())
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::ExhaustedIdentity,
            "Resource cleanup continuation identity space is exhausted");
    }

    try
    {
        PendingUnwind pending;
        pending.resources = std::move(resources);
        pending.scopes_to_close = std::move(scopes_to_close);
        pending.reason = reason;
        pending.completion_state = completion_state;
        pending.shutdown = shutdown;
        pending.continuation =
            ResourceCleanupContinuationId(next_continuation_id_);
        pending_unwind_ = std::move(pending);
        bound_dispatcher_ = &dispatcher;
        ++next_continuation_id_;
        state_ = ResourceLedgerState::Unwinding;
        return PumpUnwind(dispatcher);
    }
    catch (...)
    {
        pending_unwind_.reset();
        if (reason == ResourceReleaseReason::StateEpochChanged)
        {
            disposition_ = ResourceCleanupDisposition::TaintRequired;
            diagnostic_ =
                "Resource cleanup could not be initialized after StateEpoch replacement";
            state_ = ResourceLedgerState::Tainted;
        }
        else
        {
            state_ =
                disposition_ == ResourceCleanupDisposition::TaintRequired
                ? ResourceLedgerState::Tainted
                : completion_state;
        }
        return RejectedUnwind(
            ResourceLedgerErrorCode::InternalFailure,
            "Resource unwind could not be initialized");
    }
}

ResourceUnwindResult SessionResourceLedger::PumpUnwind(
    IResourceReleaseDispatcher& dispatcher)
{
    if (!pending_unwind_)
    {
        return RejectedUnwind(
            ResourceLedgerErrorCode::InvalidState,
            "No resource unwind is active");
    }

    PendingUnwind& pending = *pending_unwind_;
    while (pending.next_resource < pending.resources.size())
    {
        const ResourceReceiptId id =
            pending.resources[pending.next_resource];
        const auto found = std::find_if(
            resources_.begin(),
            resources_.end(),
            [id](const ResourceReceipt& candidate) {
                return candidate.id == id;
            });
        if (found == resources_.end() || !IsLive(found->status))
        {
            ++pending.next_resource;
            continue;
        }

        ResourceReleaseRequest request;
        request.receipt = *found;
        request.reason = pending.reason;
        request.current_epoch = state_epoch_;
        request.cleanup_only = true;

        const ResourceReleaseResult release = dispatcher.Release(request);
        pending.steps.push_back(ResourceUnwindStep{request, release});

        if (release.status ==
            ResourceReleaseStatus::CleanupExecutionRequired)
        {
            ResourceUnwindResult result;
            result.outcome =
                ResourceUnwindOutcome::CleanupExecutionRequired;
            result.disposition = disposition_;
            result.steps = pending.steps;
            result.rebind_requests = pending.rebind_requests;
            result.cleanup_execution_request =
                ResourceCleanupExecutionRequest{
                    pending.continuation,
                    request};
            return result;
        }

        found->release_diagnostic = release.diagnostic;
        switch (release.status)
        {
        case ResourceReleaseStatus::Released:
            found->status = ResourceRecordStatus::Released;
            break;
        case ResourceReleaseStatus::SupersededByStateReplacement:
            found->status =
                ResourceRecordStatus::SupersededByStateReplacement;
            break;
        case ResourceReleaseStatus::Failed:
            found->status = ResourceRecordStatus::ReleaseFailed;
            pending.had_failure = true;
            if (!release.diagnostic.empty())
            {
                if (!diagnostic_.empty())
                    diagnostic_ += "; ";
                diagnostic_ += release.diagnostic;
            }
            if (found->cleanup == ResourceCleanupRequirement::Mandatory)
            {
                disposition_ =
                    ResourceCleanupDisposition::TaintRequired;
            }
            else
                disposition_ =
                    MergeDiagnosticDisposition(disposition_);
            break;
        case ResourceReleaseStatus::CleanupExecutionRequired:
            break;
        }

        if (pending.reason == ResourceReleaseReason::StateEpochChanged &&
            found->epoch_policy ==
                ResourceEpochPolicy::RebindAfterRestore &&
            release.status != ResourceReleaseStatus::Failed)
        {
            pending.rebind_requests.push_back(ResourceRebindRequest{
                *found,
                found->owner,
                found->service,
                found->scope,
                found->release.kind,
                found->rebind_key,
                state_epoch_});
        }

        ++pending.next_resource;
    }

    ResourceUnwindResult result;
    result.outcome = pending.had_failure
        ? ResourceUnwindOutcome::CleanupFailed
        : ResourceUnwindOutcome::Completed;
    result.disposition = disposition_;
    result.steps = pending.steps;
    result.rebind_requests = pending.rebind_requests;
    FinishPendingUnwind();
    result.disposition = disposition_;
    return result;
}

ResourceUnwindResult SessionResourceLedger::RejectedUnwind(
    ResourceLedgerErrorCode code,
    std::string message) const
{
    ResourceUnwindResult result;
    result.outcome = ResourceUnwindOutcome::Rejected;
    result.disposition = disposition_;
    result.error = Error(code, std::move(message));
    return result;
}

ResourceOperationResult SessionResourceLedger::FailedOperation(
    ResourceLedgerErrorCode code,
    std::string message) const
{
    return ResourceOperationResult{
        false,
        Error(code, std::move(message))};
}

void SessionResourceLedger::FinishPendingUnwind()
{
    if (!pending_unwind_)
        return;

    const bool had_failure = pending_unwind_->had_failure;
    for (ResourceScopeId id : pending_unwind_->scopes_to_close)
    {
        const bool still_owns_resource = std::any_of(
            resources_.begin(),
            resources_.end(),
            [this, id](const ResourceReceipt& resource) {
                return IsLive(resource.status) &&
                    IsInScopeSubtree(resource.scope, id);
            });
        if (still_owns_resource && !pending_unwind_->shutdown)
            continue;

        const auto scope = std::find_if(
            scopes_.begin(),
            scopes_.end(),
            [id](const ScopeRecord& candidate) {
                return candidate.receipt.id == id;
            });
        if (scope != scopes_.end())
            scope->receipt.open = false;
    }

    const bool shutdown = pending_unwind_->shutdown;
    const ResourceLedgerState completion_state =
        pending_unwind_->completion_state;
    const ResourceReleaseReason reason = pending_unwind_->reason;
    std::vector<ResourceRebindRequest> rebind_requests =
        pending_unwind_->rebind_requests;
    pending_unwind_.reset();

    if (shutdown)
    {
        state_ = ResourceLedgerState::Closed;
    }
    else if (
        disposition_ == ResourceCleanupDisposition::TaintRequired)
    {
        state_ = ResourceLedgerState::Tainted;
        pending_rebinds_.clear();
        transition_origin_epoch_ = {};
    }
    else if (
        reason == ResourceReleaseReason::StateEpochChanged &&
        !rebind_requests.empty())
    {
        pending_rebinds_ = std::move(rebind_requests);
        state_ = ResourceLedgerState::StateTransition;
    }
    else
    {
        state_ = completion_state;
        if (reason == ResourceReleaseReason::StateEpochChanged ||
            had_failure)
        {
            transition_origin_epoch_ = {};
        }
    }
}

} // namespace savor::runtime
