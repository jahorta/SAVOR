#include "SessionResourceBindingTable.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace savor::runtime::program {

SessionResourceBindingTable::SessionResourceBindingTable(
    std::thread::id owner_thread) noexcept
    : owner_thread_(owner_thread)
{
}

SessionResourceBindingReceipt SessionResourceBindingTable::Bind(
    SessionResourceBindingDefinition definition)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            {},
            "Session resource binding was created outside the actor thread"};
    }
    if (!definition.release)
    {
        return {
            false,
            {},
            "Session resource binding requires a concrete release callback"};
    }
    if (definition.kind == ResourceKind::HostResource &&
        definition.diagnostic_label.empty())
    {
        return {
            false,
            {},
            "Host resource bindings require a diagnostic label"};
    }
    if (definition.rebind && !definition.rebind_key)
    {
        return {
            false,
            {},
            "A rebindable resource requires a stable rebind key"};
    }
    if (next_external_id_ == 0 ||
        next_external_id_ == std::numeric_limits<std::uint64_t>::max())
    {
        return {
            false,
            {},
            "Session resource external identity space is exhausted"};
    }

    const ResourceExternalId identity(next_external_id_++);
    try
    {
        bindings_.push_back(BindingRecord{
            identity,
            std::move(definition),
            false});
    }
    catch (...)
    {
        return {
            false,
            {},
            "Session resource binding could not be retained"};
    }
    return {true, identity, {}};
}

SessionResourceRebindReceipt SessionResourceBindingTable::Rebind(
    const ResourceRebindRequest& request,
    StateEpoch state_epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            {},
            "Session resource rebind ran outside the actor thread"};
    }
    if (!state_epoch || request.state_epoch != state_epoch)
    {
        return {
            false,
            {},
            "Session resource rebind used a stale StateEpoch"};
    }

    const auto found = std::find_if(
        bindings_.begin(),
        bindings_.end(),
        [&request](const BindingRecord& candidate) {
            return candidate.external_id ==
                request.prior_receipt.release.external_id;
        });
    if (found == bindings_.end() || !found->awaiting_rebind)
    {
        return {
            false,
            {},
            "Session resource rebind does not match a superseded binding"};
    }
    if (!found->definition.rebind)
    {
        return {
            false,
            {},
            "Session resource binding has no rebind implementation"};
    }
    if (found->definition.kind != request.kind ||
        found->definition.rebind_key != request.stable_key)
    {
        return {
            false,
            {},
            "Session resource rebind changed its kind or stable key"};
    }

    SessionResourceBindingDefinition replacement;
    std::string diagnostic;
    bool rebound = false;
    try
    {
        rebound = found->definition.rebind(
            request,
            state_epoch,
            replacement,
            diagnostic);
    }
    catch (const std::exception& ex)
    {
        diagnostic =
            std::string("Session resource rebind threw: ") + ex.what();
    }
    catch (...)
    {
        diagnostic = "Session resource rebind threw";
    }
    if (!rebound)
    {
        return {
            false,
            {},
            diagnostic.empty()
                ? "Session resource rebind failed"
                : std::move(diagnostic)};
    }

    const auto compensate_replacement =
        [&request, state_epoch](
            SessionResourceBindingDefinition& candidate,
            std::string& message) noexcept {
            if (!candidate.release)
                return;
            ResourceReceipt synthetic;
            synthetic.owner = request.owner;
            synthetic.service = request.service;
            synthetic.scope = request.scope;
            synthetic.release = {request.kind, {}};
            synthetic.acquisition_epoch = state_epoch;
            synthetic.epoch_policy =
                ResourceEpochPolicy::EndOnEpochChange;
            synthetic.cleanup =
                ResourceCleanupRequirement::Mandatory;
            synthetic.diagnostic_label =
                candidate.diagnostic_label;
            ResourceReleaseResult released;
            try
            {
                released = candidate.release({
                    .receipt = std::move(synthetic),
                    .reason = ResourceReleaseReason::Shutdown,
                    .current_epoch = state_epoch,
                    .cleanup_only = true,
                });
            }
            catch (...)
            {
                released = {
                    ResourceReleaseStatus::Failed,
                    "replacement compensation threw"};
            }
            if (released.status != ResourceReleaseStatus::Released &&
                released.status !=
                    ResourceReleaseStatus::SupersededByStateReplacement)
            {
                if (!message.empty())
                    message += "; ";
                message += released.diagnostic.empty()
                    ? "replacement compensation failed"
                    : released.diagnostic;
            }
        };

    if (replacement.kind != request.kind ||
        replacement.rebind_key != request.stable_key ||
        replacement.acquisition_epoch != state_epoch ||
        !replacement.release ||
        !replacement.rebind)
    {
        std::string message =
            "Session resource rebind returned an incompatible replacement";
        compensate_replacement(replacement, message);
        return {
            false,
            {},
            std::move(message)};
    }

    const ResourceExternalId prior = found->external_id;
    std::optional<SessionResourceBindingDefinition> compensation;
    try
    {
        compensation.emplace(replacement);
    }
    catch (...)
    {
        std::string message =
            "Session resource rebind could not retain compensation state";
        compensate_replacement(replacement, message);
        return {false, {}, std::move(message)};
    }
    SessionResourceBindingReceipt bound = Bind(std::move(replacement));
    if (!bound.success)
    {
        compensate_replacement(*compensation, bound.diagnostic);
        return {false, {}, std::move(bound.diagnostic)};
    }

    const auto prior_record = std::find_if(
        bindings_.begin(),
        bindings_.end(),
        [prior](const BindingRecord& candidate) {
            return candidate.external_id == prior;
        });
    if (prior_record != bindings_.end())
        bindings_.erase(prior_record);

    ResourceAcquisitionDefinition acquisition;
    acquisition.owner = request.owner;
    acquisition.service = request.service;
    acquisition.release = {request.kind, bound.external_id};
    acquisition.epoch_policy = ResourceEpochPolicy::RebindAfterRestore;
    acquisition.promotion = request.prior_receipt.promotion;
    acquisition.cleanup = request.prior_receipt.cleanup;
    acquisition.rebind_key = request.stable_key;
    acquisition.diagnostic_label =
        request.prior_receipt.diagnostic_label;

    return {
        true,
        ResourceRebindCompletion{
            request.prior_receipt.id,
            std::move(acquisition)},
        {}};
}

ResourceReleaseResult SessionResourceBindingTable::Release(
    const ResourceReleaseRequest& request) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            ResourceReleaseStatus::Failed,
            "Session resource release ran outside the actor thread"};
    }

    const auto found = std::find_if(
        bindings_.begin(),
        bindings_.end(),
        [&request](const BindingRecord& candidate) {
            return candidate.external_id ==
                request.receipt.release.external_id;
        });
    if (found == bindings_.end())
    {
        // A repeated ledger unwind is idempotent even after the concrete
        // binding has already been removed.
        return {ResourceReleaseStatus::Released, {}};
    }
    if (found->definition.kind != request.receipt.release.kind)
    {
        return {
            ResourceReleaseStatus::Failed,
            "Session resource release kind does not match its binding"};
    }

    ResourceReleaseResult result;
    try
    {
        result = found->definition.release(request);
    }
    catch (const std::exception& ex)
    {
        result = {
            ResourceReleaseStatus::Failed,
            std::string("Session resource release threw: ") + ex.what()};
    }
    catch (...)
    {
        result = {
            ResourceReleaseStatus::Failed,
            "Session resource release threw"};
    }

    if (result.status == ResourceReleaseStatus::Released ||
        result.status ==
            ResourceReleaseStatus::SupersededByStateReplacement)
    {
        if (request.reason == ResourceReleaseReason::StateEpochChanged &&
            request.receipt.epoch_policy ==
                ResourceEpochPolicy::RebindAfterRestore)
        {
            found->awaiting_rebind = true;
        }
        else
        {
            bindings_.erase(found);
        }
    }
    return result;
}

bool SessionResourceBindingTable::Contains(
    ResourceExternalId external_id) const noexcept
{
    return std::any_of(
        bindings_.begin(),
        bindings_.end(),
        [external_id](const BindingRecord& candidate) {
            return candidate.external_id == external_id;
        });
}

std::size_t SessionResourceBindingTable::size() const noexcept
{
    return bindings_.size();
}

bool SessionResourceBindingTable::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

} // namespace savor::runtime::program
