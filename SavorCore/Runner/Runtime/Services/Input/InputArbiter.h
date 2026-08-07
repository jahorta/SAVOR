#pragma once

#include "../../Execution/IInputAdvancePort.h"
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
struct InputNeutralWitnessIdTag;

using InputOwnerId = StrongId<InputOwnerIdTag>;
using InputPollReceiptId = StrongId<InputPollReceiptIdTag>;
using InputNeutralWitnessId = StrongId<InputNeutralWitnessIdTag>;

enum class InputBorrowPolicy : std::uint8_t
{
    PreserveHeldUntilBorrowerPublishes,
    RequireNeutralWitness,
};

enum class InputLeaseStatus : std::uint8_t
{
    Rejected,
    Active,
    Suspended,
    AwaitingNeutralAcknowledgement,
    Released,
    Invalidated,
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
    bool require_neutral_acknowledgement = true;
    bool movie_exclusive = false;
    InputBorrowPolicy borrow_policy =
        InputBorrowPolicy::PreserveHeldUntilBorrowerPublishes;
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

struct InputPublicationReceipt
{
    bool ok = false;
    InputLeaseId lease;
    InputPublicationToken publication;
    WorksetEpoch epoch;
    savor::GCInputFrame frame{};
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputAcknowledgementReceipt
{
    bool ok = false;
    bool acknowledged = false;
    InputPollReceiptId receipt;
    InputPublicationToken publication;
    WorksetEpoch epoch;
    std::uint32_t callback_count = 0;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputNeutralWitnessReceipt
{
    bool ok = false;
    InputNeutralWitnessId witness;
    InputLeaseId lease;
    InputPublicationToken publication;
    InputPollReceiptId acknowledgement;
    WorksetEpoch epoch;
    std::string message;
    InputArbiterErrorCode error = InputArbiterErrorCode::None;
};

struct InputReleaseReceipt
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

struct InputAdvanceBindingReceipt
{
    bool ok = false;
    InputAdvanceBindingId binding;
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
    std::size_t publication_count = 0;
    std::size_t binding_count = 0;
    bool movie_exclusive = false;
    bool stopped = false;
};

class InputArbiter final : public IInputAdvancePort
{
public:
    explicit InputArbiter(IInputBackendPort& backend);

    [[nodiscard]] InputLeaseReceipt Acquire(
        const InputLeaseRequest& request,
        WorksetEpoch epoch);
    [[nodiscard]] InputLeaseReceipt Borrow(
        InputLeaseId parent,
        const InputLeaseRequest& request,
        WorksetEpoch epoch,
        std::optional<InputNeutralWitnessId> neutral_witness =
            std::nullopt);
    [[nodiscard]] InputPublicationReceipt Publish(
        InputLeaseId lease,
        const savor::GCInputFrame& frame,
        WorksetEpoch epoch);
    [[nodiscard]] InputAcknowledgementReceipt Observe(
        InputLeaseId lease,
        InputPublicationToken publication,
        WorksetEpoch epoch);
    [[nodiscard]] InputNeutralWitnessReceipt ProveNeutralWitness(
        InputLeaseId lease,
        InputPublicationToken publication,
        WorksetEpoch epoch);
    [[nodiscard]] InputReleaseReceipt BeginRelease(
        InputLeaseId lease,
        WorksetEpoch epoch);
    [[nodiscard]] InputReleaseReceipt CompleteRelease(
        InputLeaseId lease,
        InputPublicationToken neutral_publication,
        WorksetEpoch epoch);

    [[nodiscard]] InputAdvanceBindingReceipt CreateAdvanceBinding(
        InputLeaseId lease,
        std::vector<savor::GCInputFrame> frames,
        WorksetEpoch epoch,
        std::uint32_t retry_limit = 1);
    [[nodiscard]] InputArbiterOperationReceipt ValidatePublication(
        const InputPublicationEvidence& publication) const noexcept;
    [[nodiscard]] InputAdvanceBindingReceipt
    CreatePublicationRelationship(
        const InputPublicationEvidence& publication);
    InputArbiterOperationReceipt RemoveAdvanceBinding(
        InputAdvanceBindingId binding) noexcept;

    InputArbiterOperationReceipt InitializeWorksetEpoch(
        WorksetEpoch epoch) noexcept;
    [[nodiscard]] InputArbiterShutdownReceipt Shutdown() noexcept;
    [[nodiscard]] InputArbiterSnapshot snapshot() const noexcept;

    [[nodiscard]] InputAdvanceReceipt Validate(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) override;
    [[nodiscard]] InputAdvanceReceipt PrepareNext(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch,
        std::uint32_t advance_ordinal) override;
    [[nodiscard]] InputAdvanceReceipt ObserveAcknowledgement(
        InputAdvanceBindingId binding,
        InputPublicationToken publication,
        WorksetEpoch epoch) override;
    [[nodiscard]] InputAdvanceReceipt Complete(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) noexcept override;
    [[nodiscard]] InputAdvanceReceipt Cancel(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) noexcept override;

private:
    struct LeaseState
    {
        InputLeaseRequest request;
        InputLeaseId id;
        WorksetEpoch epoch;
        InputLeaseStatus status = InputLeaseStatus::Active;
        std::optional<InputPublicationToken> latest_publication;
        std::optional<InputPublicationToken> pending_neutral;
    };

    struct PublicationState
    {
        InputLeaseId lease;
        WorksetEpoch epoch;
        std::uint64_t backend_sequence = 0;
        savor::GCInputFrame frame{};
    };

    struct BindingState
    {
        InputLeaseId lease;
        WorksetEpoch epoch;
        std::vector<savor::GCInputFrame> frames;
        std::uint32_t retry_limit = 1;
        std::uint32_t retry_count = 0;
        std::optional<std::uint32_t> prepared_ordinal;
        std::optional<InputPublicationToken> prepared_publication;
        std::optional<InputPublicationEvidence>
            publication_relationship;
    };

    struct NeutralWitnessState
    {
        InputLeaseId lease;
        InputPublicationToken publication;
        InputPollReceiptId acknowledgement;
        WorksetEpoch epoch;
    };

    [[nodiscard]] LeaseState* FindLease(InputLeaseId lease) noexcept;
    [[nodiscard]] const LeaseState* FindLease(InputLeaseId lease) const noexcept;
    [[nodiscard]] bool IsCurrent(WorksetEpoch epoch) const noexcept;
    [[nodiscard]] InputPublicationReceipt PublishInternal(
        LeaseState& lease,
        const savor::GCInputFrame& frame);
    [[nodiscard]] InputReleaseReceipt FinishRelease(LeaseState& lease);
    [[nodiscard]] InputAdvanceReceipt ValidateBinding(
        InputAdvanceBindingId binding,
        WorksetEpoch epoch) const;
    [[nodiscard]] bool MatchesPublication(
        const InputPublicationEvidence& publication) const noexcept;
    [[nodiscard]] InputAdvanceReceipt BindingFailure(std::string message) const;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool IsStopped() const noexcept;
    void ErasePublicationsForLease(InputLeaseId lease) noexcept;
    void EraseWitnessesForPublication(
        InputPublicationToken publication) noexcept;
    void EraseWitnessesForLease(InputLeaseId lease) noexcept;
    void EraseBindingsForLease(InputLeaseId lease) noexcept;
    void EraseLeaseState(InputLeaseId lease) noexcept;
    void ClearRetainedState() noexcept;

    IInputBackendPort& backend_;
    std::thread::id owner_thread_;
    WorksetEpoch epoch_;
    std::uint64_t next_lease_ = 1;
    std::uint64_t next_publication_ = 1;
    std::uint64_t next_poll_receipt_ = 1;
    std::uint64_t next_neutral_witness_ = 1;
    std::uint64_t next_binding_ = 1;
    std::optional<InputLeaseId> active_;
    std::vector<InputLeaseId> suspended_;
    std::unordered_map<std::uint64_t, LeaseState> leases_;
    std::unordered_map<std::uint64_t, PublicationState> publications_;
    std::unordered_map<std::uint64_t, NeutralWitnessState>
        neutral_witnesses_;
    std::unordered_map<std::uint64_t, BindingState> bindings_;
    bool stopped_ = false;
    std::optional<InputArbiterShutdownReceipt> shutdown_receipt_;
};

} // namespace savor::runtime
