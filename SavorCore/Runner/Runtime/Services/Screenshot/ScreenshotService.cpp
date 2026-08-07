#include "ScreenshotService.h"

#include <utility>

namespace savor::runtime {

ScreenshotService::ScreenshotService(IScreenshotBackendPort& backend)
    : backend_(backend),
      owner_thread_(std::this_thread::get_id())
{
}

void ScreenshotService::InitializeWorksetEpoch(WorksetEpoch epoch) noexcept
{
    if (!OnOwnerThread())
        return;
    epoch_ = epoch;
}

ScreenshotReceipt ScreenshotService::Capture(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            ScreenshotStatus::Rejected,
            {},
            epoch,
            path,
            BackendIntegrity::Preserved,
            "ScreenshotService operation used the wrong actor thread"};
    }
    const ScreenshotRequestId request(next_request_++);
    if (!epoch_ || epoch != epoch_)
        return {false, ScreenshotStatus::Rejected, request, epoch, path, BackendIntegrity::Preserved, "stale screenshot epoch"};
    if (active_)
        return {false, ScreenshotStatus::Rejected, request, epoch, path, BackendIntegrity::Preserved, "another screenshot is active"};
    if (path.empty() || timeout <= std::chrono::milliseconds::zero())
        return {false, ScreenshotStatus::Rejected, request, epoch, path, BackendIntegrity::Preserved, "path and positive timeout are required"};

    active_ = true;
    BackendResult result = backend_.Capture(path, timeout);
    active_ = false;
    ScreenshotReceipt receipt{
        result.ok,
        result.ok ? ScreenshotStatus::Complete : ScreenshotStatus::Failed,
        request,
        epoch,
        path,
        result.integrity,
        std::move(result.message)};
    last_receipt_ = receipt;
    return receipt;
}

ScreenshotReceipt ScreenshotService::Cancel(
    ScreenshotRequestId request,
    WorksetEpoch epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            ScreenshotStatus::Rejected,
            request,
            epoch,
            {},
            BackendIntegrity::Preserved,
            "ScreenshotService operation used the wrong actor thread"};
    }
    if (last_receipt_ && last_receipt_->request == request)
        return *last_receipt_;
    return {
        !active_,
        active_ ? ScreenshotStatus::Failed : ScreenshotStatus::Cancelled,
        request,
        epoch,
        {},
        active_ ? BackendIntegrity::Unknown : BackendIntegrity::Preserved,
        active_ ? "synchronous screenshot cannot be cancelled after dispatch" : std::string{}};
}

std::optional<ScreenshotReceipt> ScreenshotService::last_receipt() const
{
    if (!OnOwnerThread())
        return std::nullopt;
    return last_receipt_;
}

bool ScreenshotService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

} // namespace savor::runtime
