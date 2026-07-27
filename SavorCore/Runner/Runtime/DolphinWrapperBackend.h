#pragma once

#include "IDolphinBackend.h"
#include "Execution/IExecutionBackendPort.h"
#include "StopPoints/IPhysicalStopPointBackendPort.h"

#include <memory>

namespace savor::runtime {

enum class DolphinBackendCpuCore : std::uint8_t
{
    ProductionDefault,
    Jit64,
};

class DolphinWrapperBackend final
    : public IDolphinBackend,
      private IExecutionBackendPort,
      private IPhysicalStopPointBackendPort
{
public:
    explicit DolphinWrapperBackend(
        DolphinBackendCpuCore cpu_core = DolphinBackendCpuCore::ProductionDefault);
    ~DolphinWrapperBackend() override;

    DolphinWrapperBackend(const DolphinWrapperBackend&) = delete;
    DolphinWrapperBackend& operator=(const DolphinWrapperBackend&) = delete;

    BackendResult Open(const BackendOpenOptions& options) override;
    BackendResult Reboot() override;
    BackendResult Close() override;

    [[nodiscard]] BackendCoreState QueryCoreState() const noexcept override;
    [[nodiscard]] BackendHealthReport CheckHealth() const override;

    BackendResult RestoreStateFile(const std::filesystem::path& path) override;
    BackendResult SaveStateFile(const std::filesystem::path& path) override;
    BackendBufferResult SaveStateBuffer() override;
    BackendResult RestoreStateBuffer(const std::vector<std::uint8_t>& bytes) override;

    BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] IPhysicalStopPointBackendPort* PhysicalStopPoints() noexcept override;
    [[nodiscard]] IExecutionBackendPort* Execution() noexcept override;

private:
    [[nodiscard]] BackendExecutionCapabilityMask
    Capabilities() const noexcept override;
    [[nodiscard]] BackendExecutionSnapshot
    QueryExecutionSnapshot() const override;
    BackendResult RequestPause() override;
    BackendResult Resume() override;
    BackendResult BeginFrameStep() override;
    BackendResult BeginExactInstructionStep() override;
    BackendResult SetThrottleDisabled(bool disabled) override;

    PhysicalStopBackendReceipt BindNativeStopSink(
        savor::probe::INativeStopSink& sink) override;
    PhysicalStopBackendReceipt UnbindNativeStopSink(
        savor::probe::INativeStopSink& sink) override;
    PhysicalStopBackendReceipt QueryPhysicalStopPoints() const override;
    PhysicalStopBackendReceipt ApplyExactPhysicalStopPlan(
        const PhysicalStopPointPlan& plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    PhysicalStopBackendReceipt PublishExactPhysicalStopPlanUnchanged(
        const PhysicalStopPointPlan& expected_plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    PhysicalStopBackendReceipt RevalidatePhysicalStopPlan(
        const PhysicalStopPointPlan& plan,
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;
    PhysicalStopBackendReceipt ClearOwnedPhysicalStopPoints(
        PhysicalPlanGeneration generation,
        const std::function<void()>& commit_while_cpu_excluded) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<IDolphinBackend> MakeDolphinWrapperBackend();

} // namespace savor::runtime
