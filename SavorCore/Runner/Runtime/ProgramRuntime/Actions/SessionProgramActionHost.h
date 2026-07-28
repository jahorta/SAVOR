#pragma once

#include "ProgramActionProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace savor::runtime {

class EmulationSession;

namespace program {

struct SessionProgramActionHostConfig
{
    std::chrono::milliseconds default_action_timeout{
        std::chrono::seconds(30)};
    std::chrono::milliseconds maximum_action_timeout{
        std::chrono::minutes(10)};
    std::uint32_t maximum_cleanup_advances = 8;
    std::size_t maximum_retained_completions = 64;
};

struct SessionProgramActionHostSnapshot
{
    bool shutdown = false;
    bool invocation_active = false;
    InvocationId invocation_id;
    AttemptId attempt_id;
    StateEpoch epoch;
    std::size_t mapped_scope_count = 0;
    std::size_t mapped_resource_count = 0;
    bool execution_pending = false;
    std::size_t queued_completion_count = 0;
};

// The sole actor-owned bridge from verified ProgramRuntime requests to the
// narrow services owned by one EmulationSession. It has no thread and never
// calls a raw backend or Dolphin surface.
class SessionProgramActionHost final : public IProgramActionHost
{
public:
    explicit SessionProgramActionHost(
        EmulationSession& session,
        SessionProgramActionHostConfig config = {});
    ~SessionProgramActionHost() override;

    SessionProgramActionHost(const SessionProgramActionHost&) = delete;
    SessionProgramActionHost& operator=(
        const SessionProgramActionHost&) = delete;

    [[nodiscard]] ProgramActionDispatchResult Dispatch(
        ProgramActionRequest request) override;
    void RequestCancellation(
        InvocationId invocation_id,
        CancellationReason reason) noexcept override;
    void HandleExecutionEvent(ExecutionEvent event) override;
    void Pump() override;
    [[nodiscard]] std::vector<ProgramActionCompletion>
        DrainCompletions() override;
    void Shutdown() noexcept override;

    [[nodiscard]] SessionProgramActionHostSnapshot snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace program
} // namespace savor::runtime
