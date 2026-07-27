#include "GuestMemory.h"

#include <utility>

namespace savor::runtime {

GuestMemory::GuestMemory(IGuestMemoryBackendPort& backend)
    : backend_(backend),
      owner_thread_(std::this_thread::get_id())
{
}

void GuestMemory::CommitStateEpoch(StateEpoch epoch) noexcept
{
    if (!OnOwnerThread())
        return;
    epoch_ = epoch;
}

GuestReadReceipt GuestMemory::ReadScalar(
    std::uint32_t address,
    GuestScalarWidth width,
    StateEpoch epoch) const
{
    if (!OnOwnerThread())
    {
        return {
            false,
            epoch,
            address,
            width,
            0,
            "GuestMemory operation used the wrong actor thread"};
    }
    if (!epoch_ || epoch_ != epoch)
        return {false, epoch, address, width, 0, "stale guest-memory epoch"};
    if (!backend_.IsPaused())
        return {false, epoch, address, width, 0, "guest reads require a paused core"};

    const std::size_t size = static_cast<std::size_t>(width);
    GuestBytesResult bytes = backend_.Read(address, size);
    if (!bytes.result.ok || bytes.bytes.size() != size)
    {
        return {
            false,
            epoch,
            address,
            width,
            0,
            bytes.result.message.empty()
                ? "guest read returned an unexpected byte count"
                : std::move(bytes.result.message)};
    }

    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes.bytes)
        value = (value << 8) | byte;
    return {true, epoch, address, width, value, {}};
}

GuestBytesResult GuestMemory::ReadBytes(
    std::uint32_t address,
    std::size_t size,
    StateEpoch epoch) const
{
    if (!OnOwnerThread())
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "GuestMemory operation used the wrong actor thread"),
            {}};
    }
    if (!epoch_ || epoch_ != epoch)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "stale guest-memory epoch"),
            {}};
    }
    if (!backend_.IsPaused())
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "guest reads require a paused core"),
            {}};
    }
    return backend_.Read(address, size);
}

bool GuestMemory::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

} // namespace savor::runtime
