#pragma once

#include "Runner/Runtime/Services/State/StateTypes.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct StateBackendResult
{
    bool ok = false;
    StateIntegrity integrity = StateIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static StateBackendResult Success()
    {
        return {true, StateIntegrity::Preserved, {}};
    }

    [[nodiscard]] static StateBackendResult Failure(
        std::string message,
        StateIntegrity integrity = StateIntegrity::Preserved)
    {
        return {false, integrity, std::move(message)};
    }
};

struct StateBackendBufferResult
{
    StateBackendResult result;
    std::vector<std::uint8_t> bytes;
};

class IStateBackendPort
{
public:
    virtual ~IStateBackendPort() = default;

    virtual StateBackendResult Boot(const StateBootRequest& request) = 0;
    virtual StateBackendResult Reboot(const StateBootRequest& request) = 0;
    virtual StateBackendResult Shutdown() noexcept = 0;

    [[nodiscard]] virtual StateCompatibilityToken
    CurrentCompatibility() const = 0;

    virtual StateBackendBufferResult SaveStateBuffer() = 0;
    virtual StateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) = 0;
    virtual StateBackendResult SaveStateFile(
        const std::filesystem::path& path) = 0;
    virtual StateBackendResult RestoreStateFile(
        const std::filesystem::path& path) = 0;

protected:
    IStateBackendPort() = default;
};

class IStateReplacementParticipant
{
public:
    virtual ~IStateReplacementParticipant() = default;

    virtual StateServiceResult PrepareStateReplacement(
        const StateReplacementContext& context) = 0;
    virtual StateServiceResult CommitStateReplacement(
        const StateReplacementContext& context) = 0;
    virtual StateServiceResult RollbackStateReplacement(
        const StateReplacementContext& context) noexcept = 0;

protected:
    IStateReplacementParticipant() = default;
};

} // namespace savor::runtime
