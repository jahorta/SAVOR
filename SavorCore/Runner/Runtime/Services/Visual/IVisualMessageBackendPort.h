#pragma once

#include "../../IDolphinBackend.h"

#include <cstdint>
#include <string>

namespace savor::runtime {

enum class VisualMessageSlot : std::int32_t
{
    CurrentPhase = 1,
};

class IVisualMessageBackendPort
{
public:
    virtual ~IVisualMessageBackendPort() = default;

    IVisualMessageBackendPort(const IVisualMessageBackendPort&) = delete;
    IVisualMessageBackendPort& operator=(const IVisualMessageBackendPort&) = delete;

    [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
    virtual BackendResult ReplaceMessage(
        VisualMessageSlot slot,
        std::string message) = 0;

protected:
    IVisualMessageBackendPort() = default;
};

} // namespace savor::runtime
