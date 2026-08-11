#pragma once

#include "../../Services/Resources/SessionResourceLedger.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace savor::runtime::program {

using SessionResourceReleaseCallback =
    std::function<ResourceReleaseResult(const ResourceReleaseRequest&)>;

struct SessionResourceBindingDefinition
{
    ResourceKind kind = ResourceKind::HostResource;
    WorksetEpoch acquisition_epoch;
    SessionResourceReleaseCallback release;
    std::string diagnostic_label;
};

struct SessionResourceBindingReceipt
{
    bool success = false;
    ResourceExternalId external_id;
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

    [[nodiscard]] ResourceReleaseResult Release(
        const ResourceReleaseRequest& request) noexcept override;

    [[nodiscard]] bool Contains(ResourceExternalId external_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct BindingRecord
    {
        ResourceExternalId external_id;
        SessionResourceBindingDefinition definition;
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;

    std::thread::id owner_thread_;
    std::uint64_t next_external_id_ = 1;
    std::vector<BindingRecord> relationships_;
};

} // namespace savor::runtime::program
