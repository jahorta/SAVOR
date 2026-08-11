#pragma once

#include "../../IDolphinBackend.h"
#include "../../../../Core/Input/GCInputFrame.h"

#include <cstdint>

namespace savor::runtime {

struct BackendInputPublication
{
    BackendResult result;
    std::uint64_t publication_epoch = 0;
};

struct BackendInputPoll
{
    BackendResult result;
    std::uint64_t publication_epoch = 0;
    std::uint32_t callback_count = 0;
    savor::GCInputFrame frame{};
};

class IInputBackendPort
{
public:
    virtual ~IInputBackendPort() = default;

    IInputBackendPort(const IInputBackendPort&) = delete;
    IInputBackendPort& operator=(const IInputBackendPort&) = delete;

    [[nodiscard]] virtual bool IsAvailable(std::uint8_t port) const noexcept = 0;
    [[nodiscard]] virtual BackendInputPublication Publish(
        std::uint8_t port,
        const savor::GCInputFrame& frame) = 0;
    [[nodiscard]] virtual BackendInputPoll QueryPoll(
        std::uint8_t port) const = 0;

protected:
    IInputBackendPort() = default;
};

} // namespace savor::runtime
