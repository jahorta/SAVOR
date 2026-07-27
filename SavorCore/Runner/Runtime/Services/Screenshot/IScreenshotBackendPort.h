#pragma once

#include "../../IDolphinBackend.h"

#include <chrono>
#include <filesystem>

namespace savor::runtime {

class IScreenshotBackendPort
{
public:
    virtual ~IScreenshotBackendPort() = default;

    IScreenshotBackendPort(const IScreenshotBackendPort&) = delete;
    IScreenshotBackendPort& operator=(const IScreenshotBackendPort&) = delete;

    [[nodiscard]] virtual BackendResult Capture(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) = 0;

protected:
    IScreenshotBackendPort() = default;
};

} // namespace savor::runtime
