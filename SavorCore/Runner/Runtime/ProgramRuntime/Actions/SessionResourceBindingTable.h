#pragma once

#include "../../Services/Resources/SessionResourceLedger.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace savor::runtime::program {

struct SessionResourceBindingDefinition;

using SessionResourceReleaseCallback =
    std::function<ResourceReleaseResult(const ResourceReleaseRequest&)>;
using SessionResourceRebindCallback = std::function<bool(
    const ResourceRebindRequest&,
    StateEpoch,
    SessionResourceBindingDefinition&,
    std::string&)>;

struct SessionResourceBindingDefinition
{
    ResourceKind kind = ResourceKind::HostResource;
    StateEpoch acquisition_epoch;
    ResourceRebindKey rebind_key;
    SessionResourceReleaseCallback release;
    SessionResourceRebindCallback rebind;
    std::string diagnostic_label;
};

struct SessionResourceBindingReceipt
{
    bool success = false;
    ResourceExternalId external_id;
    std::string diagnostic;
};

struct SessionResourceRebindReceipt
{
    bool success = false;
    ResourceRebindCompletion completion;
    std::string diagnostic;
};

// The ledger deliberately stores only opaque external identities. This table
// is the actor-owned bridge from those identities to concrete service
// receipts. It is also the ledger's one release dispatcher.
class SessionResourceBindingTable final : public IResourceReleaseDispatcher
{
public:
    explicit SessionResourceBindingTable(
        std::thread::id owner_thread = std::this_thread::get_id()) noexcept;

    SessionResourceBindingTable(const SessionResourceBindingTable&) = delete;
    SessionResourceBindingTable& operator=(
        const SessionResourceBindingTable&) = delete;

    [[nodiscard]] SessionResourceBindingReceipt Bind(
        SessionResourceBindingDefinition definition);

    [[nodiscard]] SessionResourceRebindReceipt Rebind(
        const ResourceRebindRequest& request,
        StateEpoch state_epoch);

    [[nodiscard]] ResourceReleaseResult Release(
        const ResourceReleaseRequest& request) noexcept override;

    [[nodiscard]] bool Contains(ResourceExternalId external_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct BindingRecord
    {
        ResourceExternalId external_id;
        SessionResourceBindingDefinition definition;
        bool awaiting_rebind = false;
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;

    std::thread::id owner_thread_;
    std::uint64_t next_external_id_ = 1;
    std::vector<BindingRecord> bindings_;
};

} // namespace savor::runtime::program
