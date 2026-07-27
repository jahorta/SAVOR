#pragma once

#include "../../IDolphinBackend.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace savor::runtime {

struct GuestBytesResult
{
    BackendResult result;
    std::vector<std::uint8_t> bytes;
};

class IGuestMemoryBackendPort
{
public:
    virtual ~IGuestMemoryBackendPort() = default;

    IGuestMemoryBackendPort(const IGuestMemoryBackendPort&) = delete;
    IGuestMemoryBackendPort& operator=(const IGuestMemoryBackendPort&) = delete;

    [[nodiscard]] virtual bool IsPaused() const noexcept = 0;
    [[nodiscard]] virtual GuestBytesResult Read(
        std::uint32_t address,
        std::size_t size) const = 0;
    [[nodiscard]] virtual BackendResult Write(
        std::uint32_t address,
        const std::vector<std::uint8_t>& bytes) = 0;
    [[nodiscard]] virtual BackendResult InvalidateExecutableRange(
        std::uint32_t address,
        std::size_t size) = 0;

protected:
    IGuestMemoryBackendPort() = default;
};

} // namespace savor::runtime
