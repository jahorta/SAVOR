#pragma once

#include "../../RuntimeTypes.h"
#include "IScreenshotBackendPort.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace savor::runtime {

struct ScreenshotRequestIdTag;
using ScreenshotRequestId = StrongId<ScreenshotRequestIdTag>;

enum class ScreenshotStatus : std::uint8_t
{
    Rejected,
    Complete,
    Failed,
    Cancelled,
};

struct ScreenshotReceipt
{
    bool ok = false;
    ScreenshotStatus status = ScreenshotStatus::Rejected;
    ScreenshotRequestId request;
    WorksetEpoch epoch;
    std::filesystem::path path;
    BackendIntegrity integrity = BackendIntegrity::Preserved;
    std::string message;
};

class ScreenshotService final
{
public:
    explicit ScreenshotService(IScreenshotBackendPort& backend);

    void InitializeWorksetEpoch(WorksetEpoch epoch) noexcept;
    [[nodiscard]] ScreenshotReceipt Capture(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout,
        WorksetEpoch epoch);
    [[nodiscard]] ScreenshotReceipt Cancel(
        ScreenshotRequestId request,
        WorksetEpoch epoch) noexcept;
    [[nodiscard]] std::optional<ScreenshotReceipt> last_receipt() const;

private:
    [[nodiscard]] bool OnOwnerThread() const noexcept;

    IScreenshotBackendPort& backend_;
    std::thread::id owner_thread_;
    WorksetEpoch epoch_;
    std::uint64_t next_request_ = 1;
    bool active_ = false;
    std::optional<ScreenshotReceipt> last_receipt_;
};

} // namespace savor::runtime
