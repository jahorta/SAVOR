#pragma once

#include "../Actions/ProgramActionProtocol.h"
#include "../Verify/ProgramVerifier.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::program {

inline constexpr std::uint64_t kProgramExecutorQuantum = 1024;

enum class ProgramExecutorActivity : std::uint8_t
{
    Idle,
    Runnable,
    AwaitingHost,
    Unwinding,
    Terminal,
};

struct ExecutorHostRequest
{
    ProgramHostOperation operation = ProgramHostOperation::InvokeAction;
    std::optional<ExactDependencyIdentity> action;
    ProgramValueGraph input;
    ProgramScopeId scope;
    ProgramScopeId parent_scope;
    ProgramResourceHandleId resource;
    bool cleanup_only = false;
};

struct ProgramExecutorSnapshot
{
    ProgramExecutorActivity activity = ProgramExecutorActivity::Idle;
    InvocationId invocation_id;
    StateEpoch state_epoch;
    std::uint64_t instructions_executed = 0;
    std::uint64_t calls_executed = 0;
    std::uint64_t action_requests = 0;
    std::uint64_t emissions = 0;
    std::uint64_t artifacts = 0;
    std::size_t call_depth = 0;
    std::size_t scope_depth = 0;
    std::optional<ProgramActionRequestId> pending_action;
};

struct ProgramExecutorPumpResult
{
    bool runnable = false;
    std::optional<ExecutorHostRequest> host_request;
    std::optional<ProgramResult> terminal;
};

using PureReducerInvoker = std::function<std::optional<ProgramValueGraph>(
    const ExactDependencyIdentity&,
    std::span<const ProgramValueGraph>,
    std::string&)>;
using ProgramExecutorClock =
    std::function<std::chrono::steady_clock::time_point()>;

class ProgramExecutor final
{
public:
    explicit ProgramExecutor(
        PureReducerInvoker reducers = {},
        ProgramExecutorClock clock = {});
    ~ProgramExecutor();

    ProgramExecutor(const ProgramExecutor&) = delete;
    ProgramExecutor& operator=(const ProgramExecutor&) = delete;
    ProgramExecutor(ProgramExecutor&&) noexcept;
    ProgramExecutor& operator=(ProgramExecutor&&) noexcept;

    [[nodiscard]] bool Start(
        std::shared_ptr<const VerifiedProgramModule> verified,
        ProgramInvocation invocation,
        CancellationToken cancellation,
        std::string* diagnostic = nullptr);

    [[nodiscard]] ProgramExecutorPumpResult Pump(
        std::uint64_t maximum_instructions =
            kProgramExecutorQuantum);

    [[nodiscard]] bool BindPendingAction(
        ProgramActionRequestId request_id);

    [[nodiscard]] bool DeliverHostCompletion(
        ProgramActionCompletion completion,
        std::string* diagnostic = nullptr);

    [[nodiscard]] bool RequestCancellation(
        CancellationReason reason) noexcept;

    [[nodiscard]] ProgramExecutorSnapshot snapshot() const noexcept;
    [[nodiscard]] std::optional<
        std::chrono::steady_clock::time_point>
    next_wake() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::runtime::program
