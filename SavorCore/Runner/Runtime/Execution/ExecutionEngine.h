#pragma once

#include "ExecutionTypes.h"
#include "IExecutionBackendPort.h"
#include "IInputAdvancePort.h"
#include "../StopPoints/StopPointRouter.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace savor::runtime {

struct ExecutionEngineConfig
{
    std::chrono::milliseconds maintenance_interval{10};
    std::chrono::milliseconds pause_confirmation_timeout{
        std::chrono::seconds(5)};
    std::function<std::chrono::steady_clock::time_point()> now;
    std::vector<InterruptionHandlerDescriptor> interruption_handlers;
    IInputAdvancePort* input_advance = nullptr;
};

class ExecutionEngine final : private IStopPointConsumer
{
public:
    ExecutionEngine(
        IExecutionBackendPort& backend,
        StopPointRouter& stop_points,
        ExecutionEngineConfig config = {});
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    [[nodiscard]] BackendResult Initialize(StateEpoch epoch);
    [[nodiscard]] ExecutionSubmissionReceipt Submit(ExecutionRequest request);
    [[nodiscard]] ExecutionSubmissionReceipt SubmitInterruptionChild(
        InterruptionFrameId frame_id,
        ExecutionRequest request);
    [[nodiscard]] ExecutionControlReceipt Cancel(
        CancellationReason reason = CancellationReason::ExternalRequest);
    [[nodiscard]] ExecutionControlReceipt CompleteInterruptionHandler(
        InterruptionFrameId frame_id,
        InterruptionHandlerOutcome outcome,
        std::string diagnostic = {});

    void HandleStopPointReceipt(StopRouteReceipt receipt);
    void Pump();
    [[nodiscard]] std::vector<ExecutionEvent> DrainEvents();

    [[nodiscard]] ExecutionSnapshot snapshot() const;
    [[nodiscard]] bool has_active_operation() const noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_wake() const;

    [[nodiscard]] BackendResult PrepareStateReplacement();
    [[nodiscard]] BackendResult CommitStateEpoch(StateEpoch epoch);
    [[nodiscard]] BackendResult RollbackStateReplacement(StateEpoch epoch);
    [[nodiscard]] BackendResult Shutdown();

private:
    void OnStopPoint(const StopDelivery& delivery) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::runtime
