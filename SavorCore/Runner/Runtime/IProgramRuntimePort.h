#pragma once

#include "RuntimeTypes.h"
#include "ProgramRuntime/Actions/ProgramActionProtocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime {

struct ProgramModuleIdentity
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string canonical_hash;

    auto operator<=>(const ProgramModuleIdentity&) const = default;
};

struct EncodedModuleEnvelope
{
    ProgramModuleIdentity identity;
    std::uint32_t format_version = 0;
    bool development_only = false;
    std::vector<std::uint8_t> payload;

    auto operator<=>(const EncodedModuleEnvelope&) const = default;
};

struct ModuleClosureAdmissionRequest
{
    ProgramModuleIdentity root;
    std::string expected_dependency_lock_sha256;
    std::vector<EncodedModuleEnvelope> modules;
};

struct ModuleClosureAdmissionReceipt
{
    ProgramModuleIdentity root;
    std::string dependency_lock_sha256;
    std::size_t admitted_module_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !root.canonical_id.empty() &&
            dependency_lock_sha256.size() == 64 &&
            admitted_module_count > 0;
    }
};

struct EncodedInvocationEnvelope
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    ProgramModuleIdentity module;
    std::string entrypoint;
    WorksetEpoch expected_workset_epoch;
    std::vector<std::uint8_t> input_payload;
};

struct InvocationTemplatePreparationRequest
{
    WorkerCommandSequence command_sequence;
    EncodedInvocationEnvelope invocation_template;
};

struct PreparedInvocationTemplateReceipt
{
    PreparedInvocationTemplateId template_id;
    InvocationId invocation_id;
    AttemptId attempt_id;
    ProgramModuleIdentity module;
    std::string entrypoint;
    std::string program_compatibility_sha256;
    program::InvocationStatePolicy state_policy =
        program::InvocationStatePolicy::RestoreBaseline;
    std::uint64_t maximum_artifacts = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return template_id && invocation_id && attempt_id &&
            !module.canonical_id.empty() && !entrypoint.empty() &&
            program_compatibility_sha256.size() == 64;
    }
};

struct PreparedInvocationStartRequest
{
    WorkerCommandSequence command_sequence;
    PreparedInvocationTemplateId template_id;
    SessionId session_id;
    WorksetEpoch workset_epoch;
    std::string baseline_sha256;
    std::string baseline_lineage;
    bool state_already_prepared = false;
};

struct ProgramInvocationObservationEvent
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    program::ProgramEmission emission;
};

// Actor-mailbox notification that an asynchronously driven runtime has made
// its exact terminal draft available through TakeFinishedExecution. This is
// lifecycle signaling only; it is never canonical progress or telemetry.
struct ProgramInvocationCompletionAvailableEvent
{
    InvocationId invocation_id;
    AttemptId attempt_id;
};

struct ProgramInvocationTerminalEvent
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    InvocationTerminalStatus status = InvocationTerminalStatus::Failed;
    CleanupStatus cleanup = CleanupStatus::Clean;
    SessionDisposition session_disposition = SessionDisposition::Clean;
    WorksetEpoch workset_epoch;
    std::vector<std::uint8_t> output_payload;
    RuntimeError error;
    std::vector<program::ArtifactReferenceValue> workset_artifacts;
    std::vector<std::string> diagnostics;
};

// Actor-consumed execution draft. It contains the canonical ProgramResult
// before worker-owned staged outputs are finalized and appended.
struct ProgramExecutionFinished
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    InvocationTerminalStatus status = InvocationTerminalStatus::Failed;
    CleanupStatus cleanup = CleanupStatus::Clean;
    SessionDisposition session_disposition = SessionDisposition::Clean;
    WorksetEpoch workset_epoch;
    std::vector<std::uint8_t> output_payload;
    RuntimeError error;
};

struct ProgramExecutionTakeResult
{
    bool taken = false;
    std::optional<ProgramExecutionFinished> finished;
    RuntimeError error;
};

using ProgramRuntimeEvent =
    std::variant<
        ProgramInvocationObservationEvent,
        ProgramInvocationCompletionAvailableEvent>;

struct ProgramRuntimeSubmission
{
    bool accepted = false;
    // Cancellation uses this actor-arbitration result when the canonical
    // execution has already finished and its draft is retained for an exact
    // WorkerRuntime take. The exact cancellation loses without tainting or
    // rewriting that draft.
    bool execution_already_finished = false;
    RuntimeError error;

    [[nodiscard]] static ProgramRuntimeSubmission Accepted()
    {
        return {true, false, {}};
    }

    [[nodiscard]] static ProgramRuntimeSubmission
        ExecutionAlreadyFinished()
    {
        return {true, true, {}};
    }

    [[nodiscard]] static ProgramRuntimeSubmission Rejected(
        WorkerRejectionCode code,
        std::string message)
    {
        return {false, false, {code, std::move(message)}};
    }
};

class IProgramRuntimeEventSink
{
public:
    virtual ~IProgramRuntimeEventSink() = default;
    virtual void Publish(ProgramRuntimeEvent event) = 0;
};

class IProgramRuntimePort
{
public:
    virtual ~IProgramRuntimePort() = default;

    IProgramRuntimePort(const IProgramRuntimePort&) = delete;
    IProgramRuntimePort& operator=(const IProgramRuntimePort&) = delete;

    // Atomically admits and verifies a workset-supplied closed module set.
    // Workers reconstruct this in-memory cache after restart; no durable
    // program cache or database access is involved.
    virtual ProgramRuntimeSubmission AdmitModuleClosure(
        ModuleClosureAdmissionRequest request,
        ModuleClosureAdmissionReceipt& receipt)
    {
        (void)request;
        (void)receipt;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not support workset module admission");
    }

    // Workset admission validates and pins canonical invocation contents
    // before any session mutation. WorkerRuntime receives only an opaque
    // receipt and later supplies the actor-owned session/epoch binding.
    virtual ProgramRuntimeSubmission PrepareInvocationTemplate(
        InvocationTemplatePreparationRequest request,
        PreparedInvocationTemplateReceipt& receipt)
    {
        (void)request;
        (void)receipt;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not support workset templates");
    }

    virtual ProgramRuntimeSubmission ReleaseInvocationTemplate(
        PreparedInvocationTemplateId template_id)
    {
        (void)template_id;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not support workset templates");
    }

    virtual ProgramRuntimeSubmission StartPreparedInvocation(
        PreparedInvocationStartRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events)
    {
        (void)request;
        (void)cancellation;
        (void)events;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not support prepared invocations");
    }

    virtual ProgramRuntimeSubmission RequestCancellation(
        InvocationId invocation_id) = 0;

    // Program effects cross this actor-marshalled seam. A runtime may publish
    // requests from StartInvocation/Pump, but it never receives a completion
    // inline from that publication.
    virtual void BindActionSink(
        std::shared_ptr<program::IProgramActionRequestSink> sink)
    {
        (void)sink;
    }

    virtual ProgramRuntimeSubmission DeliverActionResolution(
        program::ProgramActionResolution completion)
    {
        (void)completion;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not accept actor action resolutions");
    }

    // Atomically transfers an exact retained execution draft to the worker.
    // A correlation mismatch leaves the draft retained.
    [[nodiscard]] virtual ProgramExecutionTakeResult TakeFinishedExecution(
        InvocationId invocation_id,
        AttemptId attempt_id)
    {
        (void)invocation_id;
        (void)attempt_id;
        return {false, std::nullopt, {
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not retain finished executions"}};
    }

    // Returns true when another fair actor quantum is immediately runnable.
    [[nodiscard]] virtual bool Pump()
    {
        return false;
    }

    [[nodiscard]] virtual std::optional<
        std::chrono::steady_clock::time_point>
    next_wake() const
    {
        return std::nullopt;
    }

    virtual void Shutdown() noexcept = 0;

protected:
    IProgramRuntimePort() = default;
};

} // namespace savor::runtime
