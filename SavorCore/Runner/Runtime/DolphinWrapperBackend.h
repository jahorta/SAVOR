#pragma once

#include "IDolphinBackend.h"

#include <memory>

namespace savor::runtime {

class DolphinWrapperBackend final : public IDolphinBackend
{
public:
    DolphinWrapperBackend();
    ~DolphinWrapperBackend() override;

    DolphinWrapperBackend(const DolphinWrapperBackend&) = delete;
    DolphinWrapperBackend& operator=(const DolphinWrapperBackend&) = delete;

    BackendResult Open(const BackendOpenOptions& options) override;
    BackendResult Reboot() override;
    BackendResult Close() override;

    [[nodiscard]] BackendCoreState QueryCoreState() const noexcept override;
    [[nodiscard]] BackendHealthReport CheckHealth() const override;

    BackendResult Pause(std::chrono::milliseconds timeout) override;
    BackendResult Resume() override;
    BackendResult StepInstruction(std::chrono::milliseconds timeout) override;
    BackendResult StepFrame(std::chrono::milliseconds timeout) override;

    BackendResult RestoreStateFile(const std::filesystem::path& path) override;
    BackendResult SaveStateFile(const std::filesystem::path& path) override;
    BackendBufferResult SaveStateBuffer() override;
    BackendResult RestoreStateBuffer(const std::vector<std::uint8_t>& bytes) override;

    BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<IDolphinBackend> MakeDolphinWrapperBackend();

} // namespace savor::runtime
