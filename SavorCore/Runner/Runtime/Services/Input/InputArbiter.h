#pragma once

#include "../../Execution/IInputExecutionBindingPort.h"
#include "../../RuntimeTypes.h"
#include "IInputBackendPort.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace savor::runtime {

struct InputOwnerIdTag;
struct InputPollReceiptIdTag;

using InputOwnerId = StrongId<InputOwnerIdTag>;
using InputPollReceiptId = StrongId<InputPollReceiptIdTag>;

enum class InputBorrowPolicy : std::uint8_t
{
    PreserveHeldUntilBorrowerApplies,
    RequireStableNeutral,
};

enum class InputLeaseStatus : std::uint8_t
{
    Rejected,
    Active,
    Suspended,
    Released,
    Invalidated,
};

enum class InputState : std::uint8_t
{
    Neutral,
    Held,
    DeliveryPending,
    NeutralTransitionPending,
};

enum class InputBindingKind : std::uint8_t
{
    StableNeutral,
    Held,
    Delivery,
    NeutralTransition,
};

enum class InputArbiterErrorCode : std::uint8_t
{
    None,
    WrongThread,
    Stopped,
    InvalidArgument,
    WorksetEpochMismatch,
    BackendFailure,
};

struct InputLeaseRequest
{
    InputOwnerId owner;
    std::uint8_t port = 0;
    std::int32_t priority = 0;
    bool suspendable = true;
    bool interruption_borrowable = false;
    bool movie_exclusive = false;
    InputBorrowPolicy borrow_policy =
        InputBorrowPolicy::PreserveHeldUntilBorrowerApplies;
};

struct InputLeaseReceipt
{
    bool ok = false;
    InputLeaseStatus status = InputLeaseStatus::Rejected;
    InputLeaseId lease;
    WorksetEpoch epoch;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputExecutionBindingReceipt
{
    bool ok = false;
    InputExecutionBindingId binding;
    InputLeaseId lease;
    WorksetEpoch epoch;
    std::uint64_t state_generation = 0;
    savor::GCInputFrame frame{};
    InputBindingKind kind = InputBindingKind::StableNeutral;
    bool requires_observation = false;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
    InputPublicationToken publication;
};

struct InputDeliveryReceipt
{
    bool ok = false;
    InputDeliveryId delivery;
    InputExecutionBindingId binding;
    InputLeaseId lease;
    InputPublicationToken publication;
    InputPollReceiptId poll;
    WorksetEpoch epoch;
    std::uint64_t state_generation = 0;
    std::uint32_t callback_count = 0;
    savor::GCInputFrame frame{};
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputLeaseCloseReceipt
{
    bool ok = false;
    InputLeaseStatus status = InputLeaseStatus::Rejected;
    InputLeaseId lease;
    InputPublicationToken neutral_publication;
    std::optional<InputLeaseId> resumed_lease;
    WorksetEpoch epoch;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputArbiterOperationReceipt
{
    bool ok = false;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
    std::string_view message;
};

struct InputExecutionRelationshipReceipt
{
    bool ok = false;
    InputExecutionRelationshipId relationship;
    WorksetEpoch epoch;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputArbiterShutdownReceipt
{
    bool ok = false;
    bool cleanup_complete = false;
    bool taint_required = false;
    InputPublicationToken neutral_publication;
    WorksetEpoch epoch;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputArbiterSnapshot
{
    WorksetEpoch epoch;
    std::optional<InputLeaseId> active_lease;
    std::size_t suspended_count = 0;
    std::size_t lease_count = 0;
    std::size_t binding_count = 0;
    std::size_t relationship_count = 0;
    InputState active_state = InputState::Neutral;
    bool movie_exclusive = false;
    bool stopped = false;
};

class InputArbiter final : public IInputExecutionBindingPort
{
public:
    explicit InputArbiter(IInputBackendPort& backend);

    [[nodiscard]] InputLeaseReceipt Acquire(
        const InputLeaseRequest& request,
        WorksetEpoch epoch);
    [[nodiscard]] InputLeaseReceipt Borrow(
        InputLeaseId parent,
        const InputLeaseRequest& request,
        WorksetEpoch epoch);

    [[nodiscard]] InputExecutionBindingReceipt ApplyState(
        InputLeaseId lease,
        const savor::GCInputFrame& frame,
        WorksetEpoch epoch);
    [[nodiscard]] InputExecutionBindingReceipt BeginDelivery(
        InputLeaseId lease,
        const savor::GCInputFrame& frame,
        WorksetEpoch epoch);
    [[nodiscard]] InputDeliveryReceipt CompleteDelivery(
        InputLeaseId lease,
        InputExecutionBindingId binding,
        WorksetEpoch epoch);
    [[nodiscard]] InputExecutionBindingReceipt ReplaceDelivery(
        InputLeaseId lease,
        InputExecutionBindingId observed_binding,
        const savor::GCInputFrame& replacement,
        WorksetEpoch epoch);

    [[nodiscard]] InputArbiterOperationReceipt ValidateBinding(
        const InputExecutionBindingEvidence& binding) const noexcept;
    [[nodiscard]] InputExecutionRelationshipReceipt
    CreateExecutionRelationship(
        const InputExecutionBindingEvidence& binding);
    InputArbiterOperationReceipt RemoveExecutionRelationship(
        InputExecutionRelationshipId relationship) noexcept;

    [[nodiscard]] InputLeaseCloseReceipt CloseLease(
        InputLeaseId lease,
        WorksetEpoch epoch);

    InputArbiterOperationReceipt InitializeWorksetEpoch(
        WorksetEpoch epoch) noexcept;
    [[nodiscard]] InputArbiterShutdownReceipt Shutdown() noexcept;
    [[nodiscard]] InputArbiterSnapshot snapshot() const noexcept;

    [[nodiscard]] InputExecutionRelationshipOperationReceipt Validate(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) override;
    [[nodiscard]] InputExecutionRelationshipOperationReceipt Complete(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) noexcept override;
    [[nodiscard]] InputExecutionRelationshipOperationReceipt Cancel(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) noexcept override;
    [[nodiscard]] InputExecutionRelationshipInspection Inspect(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) const noexcept override;

private:
    struct LeaseState
    {
        InputLeaseRequest request;
        InputLeaseId id;
        WorksetEpoch epoch;
        InputLeaseStatus status = InputLeaseStatus::Active;
        InputState input_state = InputState::Neutral;
        std::uint64_t state_generation = 1;
        std::optional<InputExecutionBindingId> current_binding;
        bool backend_neutral = true;
        bool mutated_backend = false;
    };

    struct BindingState
    {
        InputExecutionBindingId id;
        InputLeaseId lease;
        WorksetEpoch epoch;
        std::uint64_t state_generation = 0;
        savor::GCInputFrame frame{};
        InputBindingKind kind = InputBindingKind::StableNeutral;
        std::optional<InputPublicationToken> publication;
        std::uint64_t backend_publication_epoch = 0;
        bool requires_observation = false;
        bool observed = false;
        InputPollReceiptId poll;
        std::uint32_t callback_count = 0;
        std::optional<InputDeliveryId> delivery;
    };

    struct RelationshipState
    {
        InputExecutionRelationshipId id;
        InputExecutionBindingId binding;
        InputLeaseId lease;
        WorksetEpoch epoch;
        std::uint64_t state_generation = 0;
    };

    struct PublishedState
    {
        bool ok = false;
        InputPublicationToken publication;
        std::uint64_t backend_publication_epoch = 0;
        std::string message;
        InputArbiterErrorCode error = InputArbiterErrorCode::None;
    };

    [[nodiscard]] LeaseState* FindLease(InputLeaseId lease) noexcept;
    [[nodiscard]] const LeaseState* FindLease(InputLeaseId lease) const noexcept;
    [[nodiscard]] BindingState* FindBinding(InputExecutionBindingId binding) noexcept;
    [[nodiscard]] const BindingState* FindBinding(InputExecutionBindingId binding) const noexcept;
    [[nodiscard]] bool IsCurrent(WorksetEpoch epoch) const noexcept;
    [[nodiscard]] bool IsNeutral(const savor::GCInputFrame& frame) const noexcept;
    [[nodiscard]] PublishedState PublishBackend(
        LeaseState& lease,
        const savor::GCInputFrame& frame);
    [[nodiscard]] InputExecutionBindingReceipt BindCurrentState(
        LeaseState& lease,
        const savor::GCInputFrame& frame,
        InputBindingKind kind,
        std::optional<PublishedState> publication,
        std::optional<InputDeliveryId> delivery = std::nullopt);
    [[nodiscard]] InputLeaseCloseReceipt FinishClose(LeaseState& lease);
    [[nodiscard]] InputExecutionRelationshipOperationReceipt
    ValidateRelationship(
        InputExecutionRelationshipId relationship,
        WorksetEpoch epoch) const;
    [[nodiscard]] InputExecutionRelationshipOperationReceipt
    RelationshipFailure(std::string message) const;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool IsStopped() const noexcept;
    void EraseBindingsForLease(InputLeaseId lease) noexcept;
    void EraseRelationshipsForLease(InputLeaseId lease) noexcept;
    void EraseLeaseState(InputLeaseId lease) noexcept;
    void ClearRetainedState() noexcept;

    IInputBackendPort& backend_;
    std::thread::id owner_thread_;
    WorksetEpoch epoch_;
    std::uint64_t next_lease_ = 1;
    std::uint64_t next_binding_ = 1;
    std::uint64_t next_publication_ = 1;
    std::uint64_t next_poll_receipt_ = 1;
    std::uint64_t next_delivery_ = 1;
    std::uint64_t next_relationship_ = 1;
    std::optional<InputLeaseId> active_;
    std::vector<InputLeaseId> suspended_;
    std::unordered_map<std::uint64_t, LeaseState> leases_;
    std::unordered_map<std::uint64_t, BindingState> bindings_;
    std::unordered_map<std::uint64_t, RelationshipState> relationships_;
    bool stopped_ = false;
    std::optional<InputArbiterShutdownReceipt> shutdown_receipt_;
};

} // namespace savor::runtime
