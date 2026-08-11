#pragma once

#include "IDolphinBackend.h"
#include "Execution/IExecutionBackendPort.h"
#include "Services/Input/IInputBackendPort.h"
#include "Services/Memory/IGuestMemoryBackendPort.h"
#include "Services/Movie/IMovieBackendPort.h"
#include "Services/Screenshot/IScreenshotBackendPort.h"
#include "Services/Capture/ICaptureBackendPort.h"
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
      private IPhysicalStopPointBackendPort,
      private IInputBackendPort,
      private IGuestMemoryBackendPort,
      private IScreenshotBackendPort,
      private IMovieBackendPort,
      private ICaptureBackendPort
{
public:
    explicit DolphinWrapperBackend(
        DolphinBackendCpuCore cpu_core = DolphinBackendCpuCore::ProductionDefault);
    ~DolphinWrapperBackend() override;

    DolphinWrapperBackend(const DolphinWrapperBackend&) = delete;
    DolphinWrapperBackend& operator=(const DolphinWrapperBackend&) = delete;

    BackendResult Open(const BackendOpenOptions& options) override;
    BackendResult Close() override;

    [[nodiscard]] BackendCoreState QueryCoreState() const noexcept override;
    [[nodiscard]] BackendHealthReport CheckHealth() const override;
    [[nodiscard]] ArtifactCompatibilityToken
    SavestateCompatibility() const override;

    BackendResult RestoreStateFile(const std::filesystem::path& path) override;
    BackendResult SaveStateFile(const std::filesystem::path& path) override;
    BackendBufferResult SaveStateFileBytes() override;
    BackendBufferResult SaveStateBuffer() override;
    BackendResult RestoreStateBuffer(const std::vector<std::uint8_t>& bytes) override;

    BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] IPhysicalStopPointBackendPort* PhysicalStopPoints() noexcept override;
    [[nodiscard]] IExecutionBackendPort* Execution() noexcept override;
    [[nodiscard]] IInputBackendPort* Input() noexcept override;
    [[nodiscard]] IGuestMemoryBackendPort* GuestMemory() noexcept override;
    [[nodiscard]] IScreenshotBackendPort* Screenshots() noexcept override;
    [[nodiscard]] IMovieBackendPort* Movies() noexcept override;
    [[nodiscard]] ICaptureBackendPort* Captures() noexcept override;

private:
    [[nodiscard]] BackendExecutionCapabilityMask
    Capabilities() const noexcept override;
    [[nodiscard]] BackendExecutionSnapshot
    QueryExecutionSnapshot() const override;
    BackendResult RequestPause() override;
    BackendResult Resume() override;
    BackendResult BeginFrameStep() override;
    BackendResult SetThrottleDisabled(bool disabled) override;

    [[nodiscard]] bool IsAvailable(std::uint8_t port) const noexcept override;
    [[nodiscard]] BackendInputPublication Publish(
        std::uint8_t port,
        const savor::GCInputFrame& frame) override;
    [[nodiscard]] BackendInputPoll QueryPoll(
        std::uint8_t port) const override;

    [[nodiscard]] bool IsPaused() const noexcept override;
    [[nodiscard]] GuestBytesResult Read(
        std::uint32_t address,
        std::size_t size) const override;
    BackendResult Write(
        std::uint32_t address,
        const std::vector<std::uint8_t>& bytes) override;
    BackendResult InvalidateExecutableRange(
        std::uint32_t address,
        std::size_t size) override;

    BackendResult Capture(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;

    MoviePlaybackPrepareResult PrepareReadOnlyPlaybackForRestart(
        const std::filesystem::path& dtm_path) override;
    MovieBackendResult StopCoreForPreparedReadOnlyMovie() override;
    MovieBackendResult StartPreparedReadOnlyMovieCorePaused() override;
    MovieBackendResult ActivatePreparedReadOnlyMoviePlayback() override;
    MovieBackendResult DiscardPreparedReadOnlyMovie() noexcept override;
    MovieBackendResult StopMovie() noexcept override;
    MovieBackendResult BeginRecording() override;
    MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& dtm_path) override;
    MovieBackendResult CancelRecording() noexcept override;
    [[nodiscard]] MovieSnapshot Snapshot() const override;
    MovieCheckpointBackendResult CaptureRecordingCheckpoint() override;
    MovieBackendResult PrepareSavestateRestore(
        const SavestateMovieRestoreContext& context) override;
    MovieBackendResult CommitSavestateRestore(
        const SavestateMovieRestoreContext& context) override;
    MovieBackendResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext& context) noexcept override;

    [[nodiscard]] std::unique_ptr<ICaptureProfileAdapter>
    CreateCaptureProfileAdapter(
        const ProbeRouterAdapterConfig& config,
        std::string* error_out) override;

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
