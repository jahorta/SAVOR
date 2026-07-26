#pragma once

#include "RuntimeTypes.h"

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
    RuntimeError error;

    [[nodiscard]] static ProgramRuntimeSubmission Accepted()
    {
        return {true, {}};
    }

    [[nodiscard]] static ProgramRuntimeSubmission Rejected(
        WorkerRejectionCode code,
        std::string message)
    {
        return {false, {code, std::move(message)}};
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

    virtual ProgramRuntimeSubmission RequestCancellation(
        InvocationId invocation_id) = 0;

    virtual void Shutdown() noexcept = 0;

protected:
    IProgramRuntimePort() = default;
};

} // namespace savor::runtime
