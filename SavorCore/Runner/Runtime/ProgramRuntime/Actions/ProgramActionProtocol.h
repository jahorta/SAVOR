#pragma once

#include "../Model/ProgramModel.h"
#include "../Registry/ActionRegistry.h"
#include "../../Execution/ExecutionTypes.h"
#include "../../Services/Resources/SessionResourceLedger.h"
#include "../../Services/State/StateTypes.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime::program {

struct ProgramActionRequestIdTag;
using ProgramActionRequestId = StrongId<ProgramActionRequestIdTag>;

enum class ProgramHostOperation : std::uint8_t
{
    PrepareInvocationState,
    InvokeAction,
    OpenScope,
    CloseScope,
    PromoteResource,
    FinishInvocation,
};

struct ProgramActionRequest
{
    ProgramActionRequestId request_id;
    InvocationId invocation_id;
    AttemptId attempt_id;
    ProgramHostOperation operation = ProgramHostOperation::InvokeAction;
    StateEpoch expected_epoch;
    std::optional<ExactDependencyIdentity> action;
    ProgramValueGraph input;
    ProgramScopeId scope;
    ProgramScopeId parent_scope;
    ProgramResourceHandleId resource;
    std::optional<InvocationStateRequest> state_request;
    // Workset admission may prepare the complete baseline before binding the
    // invocation. The host still opens the invocation resource scope and
    // validates the exact session/epoch/lineage, but must not restore state a
    // second time.
    bool state_already_prepared = false;
    std::string prepared_baseline_sha256;
    // Guest-dependent actions are cancellation-driven. Only verified
    // host-only actions carry a finite infrastructure deadline.
    ActionTimingClass timing = ActionTimingClass::BoundedHostOperation;
    std::optional<std::chrono::steady_clock::time_point>
        bounded_host_deadline;
    ActionEffectMask allowed_effects = ~ActionEffectMask{0};
    bool cleanup_only = false;
};

enum class ProgramActionCompletionStatus : std::uint8_t
{
    Completed,
    Rejected,
    Cancelled,
    TimedOut,
    StaleEpoch,
    Unsupported,
    Failed,
    CleanupFailed,
};

struct ProgramActionResource
{
    ProgramResourceHandleId handle;
    ResourceReceiptId receipt;
    ResourceKind kind = ResourceKind::HostResource;
    StateEpoch acquisition_epoch;
    // Epoch-agnostic services reconcile their concrete object internally, so
    // the program handle must not be rejected merely because guest state was
    // replaced after acquisition.
    std::optional<StateEpoch> origin_epoch;
};

// Internal ownership transfer from a completed state-save action to the
// worker's host-output pipeline. Program IR receives only the matching typed
// pending-publication receipt. It never receives an ArtifactReferenceValue;
// only finalizer evidence can create a complete authoritative reference in
// the terminal ProgramResult.
struct PendingStateArtifactPublication
{
    std::string artifact_id;
    ImmutableStateArtifactCaptureReceipt capture;
};

struct ProgramActionCompletion
{
    ProgramActionRequestId request_id;
    InvocationId invocation_id;
    AttemptId attempt_id;
    ProgramHostOperation operation = ProgramHostOperation::InvokeAction;
    ProgramActionCompletionStatus status =
        ProgramActionCompletionStatus::Failed;
    StateEpoch origin_epoch;
    StateEpoch resulting_epoch;
    ProgramValueGraph output;
    std::vector<PendingStateArtifactPublication>
        pending_state_artifacts;
    std::vector<ProgramActionResource> resources;
    std::vector<CleanupReceipt> cleanup_receipts;
    ProgramCleanupStatus cleanup = ProgramCleanupStatus::Clean;
    SessionDisposition session_disposition = SessionDisposition::Clean;
    std::string code;
    std::string message;
};

class IProgramActionRequestSink
{
public:
    virtual ~IProgramActionRequestSink() = default;
    virtual void Publish(ProgramActionRequest request) = 0;
};

struct ProgramActionDispatchResult
{
    bool accepted = false;
    std::optional<ProgramActionCompletion> immediate_completion;
    std::string diagnostic;
};

class IProgramActionHost
{
public:
    virtual ~IProgramActionHost() = default;

    IProgramActionHost(const IProgramActionHost&) = delete;
    IProgramActionHost& operator=(const IProgramActionHost&) = delete;

    [[nodiscard]] virtual ProgramActionDispatchResult Dispatch(
        ProgramActionRequest request) = 0;
    virtual void RequestCancellation(
        InvocationId invocation_id,
        CancellationReason reason) noexcept = 0;
    virtual void HandleExecutionEvent(ExecutionEvent event) = 0;
    virtual void Pump() = 0;
    [[nodiscard]] virtual std::vector<ProgramActionCompletion>
        DrainCompletions() = 0;
    virtual void Shutdown() noexcept = 0;

protected:
    IProgramActionHost() = default;
};

} // namespace savor::runtime::program
