#include "SessionVisualMessageService.h"

#include <algorithm>
#include <utility>

namespace savor::runtime {
namespace {

constexpr std::size_t MaximumPhaseNameBytes = 120;

[[nodiscard]] bool PrintableAscii(std::string_view value) noexcept
{
    return !value.empty() && value.size() <= MaximumPhaseNameBytes &&
        std::ranges::all_of(value, [](const unsigned char character) {
            return character >= 0x20 && character <= 0x7e;
        });
}

} // namespace

SessionVisualMessageService::SessionVisualMessageService(
    IVisualMessageBackendPort& backend,
    SessionVisualMessageOptions options)
    : backend_(backend),
      options_(options),
      owner_thread_(std::this_thread::get_id())
{
}

BackendResult SessionVisualMessageService::SetCurrentPhase(
    const std::string_view display_name)
{
    if (!PrintableAscii(display_name))
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "Visual current-phase name must be 1..120 printable ASCII bytes");
    }
    return ReplaceCurrentPhase(
        "Current phase: " + std::string(display_name));
}

BackendResult SessionVisualMessageService::SetIdle()
{
    return ReplaceCurrentPhase("Current phase: Idle");
}

BackendResult SessionVisualMessageService::ReplaceCurrentPhase(
    std::string message)
{
    if (!OnOwnerThread())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "SessionVisualMessageService used outside its owner thread");
    }
    if (!options_.show_current_phase || !backend_.IsAvailable())
        return BackendResult::Success();
    if (message == current_phase_message_)
        return BackendResult::Success();

    BackendResult result = backend_.ReplaceMessage(
        VisualMessageSlot::CurrentPhase,
        message);
    if (result.ok)
        current_phase_message_ = std::move(message);
    return result;
}

bool SessionVisualMessageService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

} // namespace savor::runtime
