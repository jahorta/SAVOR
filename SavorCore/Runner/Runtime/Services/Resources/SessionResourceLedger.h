#pragma once

#include "../../RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace savor::runtime {

struct ResourceOwnerIdTag;
struct ResourceServiceIdTag;
struct ResourceScopeIdTag;
struct ResourceReceiptIdTag;
struct ResourceAcquisitionSequenceTag;
struct ResourceExternalIdTag;
struct ResourceCleanupContinuationIdTag;

using ResourceOwnerId = StrongId<ResourceOwnerIdTag>;
using ResourceServiceId = StrongId<ResourceServiceIdTag>;
using ResourceScopeId = StrongId<ResourceScopeIdTag>;
using ResourceReceiptId = StrongId<ResourceReceiptIdTag>;
using ResourceAcquisitionSequence =
    StrongId<ResourceAcquisitionSequenceTag>;
using ResourceExternalId = StrongId<ResourceExternalIdTag>;
using ResourceCleanupContinuationId =
    StrongId<ResourceCleanupContinuationIdTag>;

static_assert(!std::is_convertible_v<ResourceOwnerId, ResourceScopeId>);
static_assert(!std::is_convertible_v<ResourceOwnerId, ResourceServiceId>);
static_assert(!std::is_convertible_v<ResourceReceiptId, ResourceExternalId>);
static_assert(
    !std::is_convertible_v<ResourceAcquisitionSequence, ResourceReceiptId>);
static_assert(
    !std::is_convertible_v<
        ResourceCleanupContinuationId,
        ResourceReceiptId>);

enum class ResourceScopeKind : std::uint8_t
{
    SessionRoot,
    Synthetic,
};

enum class ResourceKind : std::uint8_t
{
    InputLease,
    StopPointGroup,
    ExecutionOperation,
    GuestMutation,
    PreparedMoviePlayback,
    MovieSession,
    CaptureAttachment,
    ArtifactWriter,
    TelemetrySubscription,
    HostResource,
};

enum class ResourcePromotionPolicy : std::uint8_t
{
    Forbidden,
    ImmediateParent,
    AnyAncestor,
};

enum class ResourceCleanupRequirement : std::uint8_t
{
    Optional,
    Mandatory,
};

enum class ResourceRecordStatus : std::uint8_t
{
    Active,
    Released,
    ReleaseFailed,
};

enum class ResourceLedgerState : std::uint8_t
{
    Uninitialized,
    Accepting,
    Unwinding,
    Tainted,
    Closed,
};

enum class ResourceLedgerErrorCode : std::uint16_t
{
    None,
    WrongThread,
    InvalidState,
    InvalidArgument,
    StaleEpoch,
    ExhaustedIdentity,
    ScopeNotFound,
    ScopeClosed,
    ResourceNotFound,
    ResourceAlreadyRegistered,
    PromotionForbidden,
    DestinationNotAncestor,
    CleanupInProgress,
    InternalFailure,
};

struct ResourceLedgerError
{
    ResourceLedgerErrorCode code = ResourceLedgerErrorCode::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != ResourceLedgerErrorCode::None;
    }
};

struct ResourceReleaseDescriptor
{
    ResourceKind kind = ResourceKind::HostResource;
    ResourceExternalId external_id;

    friend bool operator==(
        const ResourceReleaseDescriptor&,
        const ResourceReleaseDescriptor&) = default;
};

struct ResourceAcquisitionDefinition
{
    ResourceOwnerId owner;
    ResourceServiceId service;
    ResourceReleaseDescriptor release;
    ResourcePromotionPolicy promotion =
        ResourcePromotionPolicy::Forbidden;
    ResourceCleanupRequirement cleanup =
        ResourceCleanupRequirement::Mandatory;
    std::string diagnostic_label;
};

struct ResourceReceipt
{
    SessionId session;
    ResourceReceiptId id;
    ResourceAcquisitionSequence sequence;
    ResourceOwnerId owner;
    ResourceServiceId service;
    ResourceScopeId scope;
    ResourceReleaseDescriptor release;
    WorksetEpoch acquisition_epoch;
    ResourcePromotionPolicy promotion =
        ResourcePromotionPolicy::Forbidden;
    ResourceCleanupRequirement cleanup =
        ResourceCleanupRequirement::Mandatory;
    ResourceRecordStatus status = ResourceRecordStatus::Active;
    std::string diagnostic_label;
    std::string release_diagnostic;
};

struct ResourceScopeReceipt
{
    SessionId session;
    ResourceScopeId id;
    ResourceScopeId parent;
    ResourceOwnerId owner;
    ResourceScopeKind kind = ResourceScopeKind::Synthetic;
    bool open = false;
    std::string diagnostic_label;
};

struct ResourceOperationResult
{
    bool success = false;
    ResourceLedgerError error;
};

struct ResourceScopeResult
{
    bool success = false;
    ResourceScopeReceipt scope;
    ResourceLedgerError error;
};

struct ResourceAcquisitionResult
{
    bool success = false;
    std::vector<ResourceReceipt> receipts;
    ResourceLedgerError error;
};

struct ResourcePromotionResult
{
    bool success = false;
    bool changed = false;
    ResourceReceipt receipt;
    ResourceLedgerError error;
};

enum class ResourceReleaseReason : std::uint8_t
{
    Explicit,
    ScopeExit,
    Shutdown,
};

struct ResourceReleaseRequest
{
    ResourceReceipt receipt;
    ResourceReleaseReason reason = ResourceReleaseReason::Explicit;
    WorksetEpoch current_epoch;
    bool cleanup_only = true;
};

enum class ResourceReleaseStatus : std::uint8_t
{
    Released,
    CleanupExecutionRequired,
    Failed,
};

struct ResourceReleaseResult
{
    ResourceReleaseStatus status = ResourceReleaseStatus::Released;
    std::string diagnostic;
};

class IResourceReleaseDispatcher
{
public:
    virtual ~IResourceReleaseDispatcher() = default;

    [[nodiscard]] virtual ResourceReleaseResult Release(
        const ResourceReleaseRequest& request) noexcept = 0;
};

struct ResourceUnwindStep
{
    ResourceReleaseRequest request;
    ResourceReleaseResult result;
};

enum class ResourceUnwindOutcome : std::uint8_t
{
    Completed,
    AlreadyComplete,
    CleanupExecutionRequired,
    CleanupFailed,
    Rejected,
};

enum class ResourceCleanupDisposition : std::uint8_t
{
    Clean,
    CleanWithDiagnostics,
    TaintRequired,
};

struct ResourceCleanupExecutionRequest
{
    ResourceCleanupContinuationId continuation;
    ResourceReleaseRequest release;
};

struct ResourceUnwindResult
{
    ResourceUnwindOutcome outcome = ResourceUnwindOutcome::Rejected;
    ResourceCleanupDisposition disposition =
        ResourceCleanupDisposition::Clean;
    std::vector<ResourceUnwindStep> steps;
    std::optional<ResourceCleanupExecutionRequest>
        cleanup_execution_request;
    ResourceLedgerError error;

    [[nodiscard]] bool completed() const noexcept
    {
        return outcome == ResourceUnwindOutcome::Completed ||
            outcome == ResourceUnwindOutcome::AlreadyComplete;
    }
};

struct ResourceLedgerSnapshot
{
    ResourceLedgerState state = ResourceLedgerState::Uninitialized;
    ResourceCleanupDisposition disposition =
        ResourceCleanupDisposition::Clean;
    SessionId session;
    WorksetEpoch workset_epoch;
    ResourceScopeId session_root;
    std::size_t open_scope_count = 0;
    std::size_t active_resource_count = 0;
    std::size_t failed_release_count = 0;
    bool cleanup_execution_pending = false;
    std::string diagnostic;
};

class SessionResourceLedger final
{
public:
    explicit SessionResourceLedger(
        std::thread::id owner_thread = std::this_thread::get_id()) noexcept;
    ~SessionResourceLedger() = default;

    SessionResourceLedger(const SessionResourceLedger&) = delete;
    SessionResourceLedger& operator=(const SessionResourceLedger&) = delete;
    SessionResourceLedger(SessionResourceLedger&&) = delete;
    SessionResourceLedger& operator=(SessionResourceLedger&&) = delete;

    [[nodiscard]] ResourceOperationResult Initialize(
        SessionId session,
        WorksetEpoch workset_epoch,
        ResourceOwnerId session_owner);

    [[nodiscard]] ResourceScopeResult OpenSyntheticScope(
        ResourceScopeId parent,
        ResourceOwnerId owner,
        std::string diagnostic_label = {});

    [[nodiscard]] ResourceAcquisitionResult Acquire(
        ResourceScopeId scope,
        const std::vector<ResourceAcquisitionDefinition>& definitions);

    [[nodiscard]] ResourcePromotionResult Promote(
        ResourceReceiptId resource,
        ResourceScopeId destination_scope);

    [[nodiscard]] ResourceUnwindResult Release(
        ResourceReceiptId resource,
        IResourceReleaseDispatcher& dispatcher);

    [[nodiscard]] ResourceUnwindResult CloseScope(
        ResourceScopeId scope,
        IResourceReleaseDispatcher& dispatcher);

    [[nodiscard]] ResourceUnwindResult ContinueCleanup(
        ResourceCleanupContinuationId continuation,
        IResourceReleaseDispatcher& dispatcher);

    [[nodiscard]] ResourceUnwindResult Shutdown(
        IResourceReleaseDispatcher& dispatcher);

    // The ledger is actor-owned. These read-only views are valid only on the
    // owner thread unless the caller has otherwise stopped actor mutation.
    [[nodiscard]] ResourceLedgerSnapshot snapshot() const noexcept;
    [[nodiscard]] std::optional<ResourceReceipt> FindResource(
        ResourceReceiptId resource) const;
    [[nodiscard]] std::optional<ResourceScopeReceipt> FindScope(
        ResourceScopeId scope) const;

private:
    struct ScopeRecord
    {
        ResourceScopeReceipt receipt;
    };

    struct PendingUnwind
    {
        std::vector<ResourceReceiptId> resources;
        std::size_t next_resource = 0;
        std::vector<ResourceScopeId> scopes_to_close;
        ResourceReleaseReason reason = ResourceReleaseReason::Explicit;
        ResourceLedgerState completion_state = ResourceLedgerState::Accepting;
        bool shutdown = false;
        bool had_failure = false;
        ResourceCleanupContinuationId continuation;
        std::vector<ResourceUnwindStep> steps;
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] ResourceLedgerError ValidateAccepting() const;
    [[nodiscard]] ResourceLedgerError ValidateScopeForAcquisition(
        ResourceScopeId scope) const;
    [[nodiscard]] bool IsAncestor(
        ResourceScopeId possible_ancestor,
        ResourceScopeId scope) const;
    [[nodiscard]] bool IsInScopeSubtree(
        ResourceScopeId candidate,
        ResourceScopeId subtree_root) const;
    [[nodiscard]] ResourceScopeId ParentOf(ResourceScopeId scope) const;
    [[nodiscard]] ResourceUnwindResult StartUnwind(
        std::vector<ResourceReceiptId> resources,
        std::vector<ResourceScopeId> scopes_to_close,
        ResourceReleaseReason reason,
        ResourceLedgerState completion_state,
        bool shutdown,
        IResourceReleaseDispatcher& dispatcher);
    [[nodiscard]] ResourceUnwindResult PumpUnwind(
        IResourceReleaseDispatcher& dispatcher);
    [[nodiscard]] ResourceUnwindResult RejectedUnwind(
        ResourceLedgerErrorCode code,
        std::string message) const;
    [[nodiscard]] ResourceOperationResult FailedOperation(
        ResourceLedgerErrorCode code,
        std::string message) const;
    void FinishPendingUnwind();

    std::thread::id owner_thread_;
    ResourceLedgerState state_ = ResourceLedgerState::Uninitialized;
    ResourceCleanupDisposition disposition_ =
        ResourceCleanupDisposition::Clean;
    SessionId session_id_;
    WorksetEpoch workset_epoch_;
    ResourceScopeId session_root_;
    std::uint64_t next_scope_id_ = 1;
    std::uint64_t next_resource_id_ = 1;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t next_continuation_id_ = 1;
    std::vector<ScopeRecord> scopes_;
    std::vector<ResourceReceipt> resources_;
    std::optional<PendingUnwind> pending_unwind_;
    // The first cleanup operation binds the composite dispatcher. It must
    // outlive the ledger and every later cleanup must use the same instance.
    IResourceReleaseDispatcher* bound_dispatcher_ = nullptr;
    std::string diagnostic_;
};

} // namespace savor::runtime
