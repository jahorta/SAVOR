#pragma once

#include "ExecutionTypes.h"
#include "HostActivityTracker.h"
#include "IExecutionBackendPort.h"
#include "IInputExecutionBindingPort.h"
#include "../StopPoints/StopPointRouter.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace savor::runtime {

class MovieService;

struct ExecutionEngineConfig
{
    std::chrono::milliseconds maintenance_interval{10};
    std::chrono::milliseconds pause_confirmation_timeout{
        std::chrono::seconds(5)};
    std::function<std::chrono::steady_clock::time_point()> now;
    std::vector<InterruptionHandlerDescriptor> interruption_handlers;
    IInputExecutionBindingPort* input_relationships = nullptr;
    HostActivityTracker* host_activity = nullptr;
    std::chrono::milliseconds suspect_core_stall_after{
        std::chrono::seconds(10)};
    std::chrono::milliseconds confirm_core_stall_after{
        std::chrono::seconds(10)};
    std::chrono::milliseconds host_activity_warning_after{
        std::chrono::seconds(10)};
    std::chrono::milliseconds host_activity_warning_repeat{
        std::chrono::seconds(30)};
};

class ExecutionEngine final : private IStopPointConsumer
{
public:
    ExecutionEngine(
        IExecutionBackendPort& backend,
        MovieService& movies,
        StopPointRouter& stop_points,
        ExecutionEngineConfig config = {});
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    [[nodiscard]] BackendResult Initialize(WorksetEpoch epoch);
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

    [[nodiscard]] BackendResult Shutdown();

private:
    void OnStopPoint(const StopDelivery& delivery) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::runtime
