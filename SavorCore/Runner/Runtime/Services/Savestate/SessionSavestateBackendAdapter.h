#pragma once

#include "Runner/Runtime/IDolphinBackend.h"
#include "Runner/Runtime/Services/Savestate/ISavestateBackendPort.h"

namespace savor::runtime {

class SessionSavestateBackendAdapter final : public ISavestateBackendPort
{
public:
    explicit SessionSavestateBackendAdapter(IDolphinBackend& backend) noexcept;

    SessionSavestateBackendAdapter(const SessionSavestateBackendAdapter&) = delete;
    SessionSavestateBackendAdapter& operator=(
        const SessionSavestateBackendAdapter&) = delete;

    [[nodiscard]] ArtifactCompatibilityToken
    CurrentCompatibility() const override;

    SavestateBackendBufferResult SaveStateBuffer() override;
    SavestateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override;
    SavestateBackendResult SaveStateFile(
        const std::filesystem::path& path) override;
    SavestateBackendResult RestoreStateFile(
        const std::filesystem::path& path) override;

private:
    IDolphinBackend& backend_;
};

} // namespace savor::runtime
