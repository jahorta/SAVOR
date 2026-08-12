#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace savor::runtime {

enum class HitTimeGuestReadError : std::uint8_t
{
    None,
    InvalidArgument,
    BackendUnavailable,
    UnmappedRange,
};

struct HitTimeGuestReadReceipt
{
    bool ok = false;
    HitTimeGuestReadError error = HitTimeGuestReadError::BackendUnavailable;
    std::uint32_t address = 0;
    std::size_t size = 0;
};

// Private native-hook facet. Calls are valid only while StopPointRouter is
// synchronously dispatching a physical hit on Dolphin's CPU thread.
class IHitTimeGuestMemoryBackendPort
{
public:
    virtual ~IHitTimeGuestMemoryBackendPort() = default;

    IHitTimeGuestMemoryBackendPort(
        const IHitTimeGuestMemoryBackendPort&) = delete;
    IHitTimeGuestMemoryBackendPort& operator=(
        const IHitTimeGuestMemoryBackendPort&) = delete;

    [[nodiscard]] virtual HitTimeGuestReadReceipt ReadHitTimeBytes(
        std::uint32_t address,
        std::span<std::uint8_t> destination) const noexcept = 0;

protected:
    IHitTimeGuestMemoryBackendPort() = default;
};

} // namespace savor::runtime
