#pragma once

#include "Runner/Runtime/Services/Savestate/SavestateTypes.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct SavestateBackendResult
{
    bool ok = false;
    GuestIntegrity integrity = GuestIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static SavestateBackendResult Success()
    {
        return {true, GuestIntegrity::Preserved, {}};
    }

    [[nodiscard]] static SavestateBackendResult Failure(
        std::string message,
        GuestIntegrity integrity = GuestIntegrity::Preserved)
    {
        return {false, integrity, std::move(message)};
    }
};

struct SavestateBackendBufferResult
{
    SavestateBackendResult result;
    std::vector<std::uint8_t> bytes;
};

class ISavestateBackendPort
{
public:
    virtual ~ISavestateBackendPort() = default;

    [[nodiscard]] virtual ArtifactCompatibilityToken
    CurrentCompatibility() const = 0;

    virtual SavestateBackendBufferResult SaveStateBuffer() = 0;
    // Returns bytes read back from a completed native Dolphin savestate file,
    // never the raw SaveToBuffer representation.
    virtual SavestateBackendBufferResult SaveStateFileBytes() = 0;
    virtual SavestateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) = 0;
    virtual SavestateBackendResult SaveStateFile(
        const std::filesystem::path& path) = 0;
    virtual SavestateBackendResult RestoreStateFile(
        const std::filesystem::path& path) = 0;

protected:
    ISavestateBackendPort() = default;
};

} // namespace savor::runtime
