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
};

struct EncodedInvocationEnvelope
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    ProgramModuleIdentity module;
    std::string entrypoint;
    StateEpoch expected_state_epoch;
    std::vector<std::uint8_t> input_payload;
};

struct ModulePreparationRequest
{
    WorkerCommandSequence command_sequence;
    EncodedModuleEnvelope module;
};

struct ProgramInvocationRequest
{
    WorkerCommandSequence command_sequence;
    EncodedInvocationEnvelope invocation;
    bool state_already_prepared = false;
    std::string prepared_baseline_sha256;
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
        program::InvocationStatePolicy::Boot;
    std::chrono::milliseconds active_budget{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return template_id && invocation_id && attempt_id &&
            !module.canonical_id.empty() && !entrypoint.empty() &&
            program_compatibility_sha256.size() == 64 &&
            active_budget.count() > 0;
    }
};

struct PreparedInvocationStartRequest
{
    WorkerCommandSequence command_sequence;
    PreparedInvocationTemplateId template_id;
    SessionId session_id;
    StateEpoch state_epoch;
    std::string baseline_sha256;
    std::string baseline_lineage;
};

struct ProgramRuntimeCatalogModule
{
    ProgramModuleIdentity identity;
    std::vector<std::string> entrypoints;
    bool development_only = false;

    auto operator<=>(const ProgramRuntimeCatalogModule&) const = default;
};

struct ProgramRuntimeCatalogSnapshot
{
    bool complete_exact = false;
    std::uint64_t generation = 1;
    std::string runtime_profile_sha256;
    std::string dependency_manifest_sha256;
    std::string catalog_sha256;
    std::vector<ProgramRuntimeCatalogModule> modules;
};

struct ModulePreparationEvent
{
    WorkerCommandSequence command_sequence;
    ProgramModuleIdentity module;
    bool prepared = false;
    RuntimeError error;
};

struct ProgramInvocationProgressEvent
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    std::uint64_t progress_sequence = 0;
    std::string text;
    bool record_progress = true;
};

struct ProgramInvocationTerminalEvent
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    InvocationTerminalStatus status = InvocationTerminalStatus::Failed;
    CleanupStatus cleanup = CleanupStatus::Clean;
    SessionDisposition session_disposition = SessionDisposition::Clean;
    StateEpoch origin_state_epoch;
    std::vector<std::uint8_t> output_payload;
    RuntimeError error;
};

using ProgramRuntimeEvent = std::variant<
    ModulePreparationEvent,
    ProgramInvocationProgressEvent,
    ProgramInvocationTerminalEvent>;

struct ProgramRuntimeSubmission
{
    bool accepted = false;
    // Cancellation uses this actor-arbitration result when the canonical
    // terminal was already published but has not yet been consumed by
    // WorkerRuntime. The exact cancellation loses without tainting or
    // rewriting the terminal.
    bool terminal_already_published = false;
    RuntimeError error;

    [[nodiscard]] static ProgramRuntimeSubmission Accepted()
    {
        return {true, false, {}};
    }

    [[nodiscard]] static ProgramRuntimeSubmission
        TerminalAlreadyPublished()
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

    [[nodiscard]] virtual WorkerCapabilityMask capabilities() const noexcept = 0;

    // This is an invocation-lifecycle boundary, not a session-service escape hatch.
    // Implementations must not retain or obtain EmulationSession/IDolphinBackend.
    // Future program effects return to the WorkerRuntime actor through a separate
    // actor-marshalled service request surface.
    virtual ProgramRuntimeSubmission PrepareModule(
        ModulePreparationRequest request,
        std::shared_ptr<IProgramRuntimeEventSink> events) = 0;

    virtual ProgramRuntimeSubmission StartInvocation(
        ProgramInvocationRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events) = 0;

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

    [[nodiscard]] virtual ProgramRuntimeCatalogSnapshot catalog() const
    {
        return {};
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

    virtual ProgramRuntimeSubmission DeliverActionCompletion(
        program::ProgramActionCompletion completion)
    {
        (void)completion;
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::Unsupported,
            "ProgramRuntime does not accept actor action completions");
    }

    // Transfers host-only publications which were promoted by accepted
    // action completions. ProgramRuntime and ProgramExecutor never finalize
    // files or expose these pending receipts to program IR.
    [[nodiscard]] virtual std::vector<
        program::PendingStateArtifactPublication>
        DrainPendingStateArtifactPublications()
    {
        return {};
    }

    // A terminal remains correlated inside the runtime until the actor has
    // consumed it. Ports that publish terminals synchronously but do not need
    // retention may accept this acknowledgement as a no-op.
    virtual ProgramRuntimeSubmission AcknowledgeTerminal(
        InvocationId invocation_id,
        AttemptId attempt_id)
    {
        (void)invocation_id;
        (void)attempt_id;
        return ProgramRuntimeSubmission::Accepted();
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
