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
        relationships_.push_back(BindingRecord{
            identity,
            std::move(definition)});
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
        relationships_.begin(),
        relationships_.end(),
        [&request](const BindingRecord& candidate) {
            return candidate.external_id ==
                request.receipt.release.external_id;
        });
    if (found == relationships_.end())
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

    if (result.status == ResourceReleaseStatus::Released)
        relationships_.erase(found);
    return result;
}

bool SessionResourceBindingTable::Contains(
    ResourceExternalId external_id) const noexcept
{
    return std::any_of(
        relationships_.begin(),
        relationships_.end(),
        [external_id](const BindingRecord& candidate) {
            return candidate.external_id == external_id;
        });
}

std::size_t SessionResourceBindingTable::size() const noexcept
{
    return relationships_.size();
}

bool SessionResourceBindingTable::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

} // namespace savor::runtime::program
