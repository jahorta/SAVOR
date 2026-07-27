#pragma once

#include "Runner/Runtime/IDolphinBackend.h"
#include "Runner/Runtime/Services/State/IStateBackendPort.h"

namespace savor::runtime {

class SessionStateBackendAdapter final : public IStateBackendPort
{
public:
    explicit SessionStateBackendAdapter(IDolphinBackend& backend) noexcept;

    SessionStateBackendAdapter(const SessionStateBackendAdapter&) = delete;
    SessionStateBackendAdapter& operator=(
        const SessionStateBackendAdapter&) = delete;

    void ConfigureOpen(BackendOpenOptions options);

    StateBackendResult Boot(const StateBootRequest& request) override;
    StateBackendResult Reboot(const StateBootRequest& request) override;
    StateBackendResult Shutdown() noexcept override;

    [[nodiscard]] StateCompatibilityToken
    CurrentCompatibility() const override;

    StateBackendBufferResult SaveStateBuffer() override;
    StateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override;
    StateBackendResult SaveStateFile(
        const std::filesystem::path& path) override;
    StateBackendResult RestoreStateFile(
        const std::filesystem::path& path) override;

private:
    [[nodiscard]] static StateBackendResult Convert(
        BackendResult result) noexcept;

    IDolphinBackend& backend_;
    BackendOpenOptions open_options_;
    bool configured_ = false;
};

} // namespace savor::runtime
