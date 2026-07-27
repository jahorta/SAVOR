#include "Runner/Runtime/Services/State/SessionStateBackendAdapter.h"

#include <exception>
#include <utility>

namespace savor::runtime {
namespace {

template <typename Operation>
StateBackendResult Invoke(Operation&& operation) noexcept
{
    try
    {
        BackendResult result = operation();
        if (result.ok)
            return StateBackendResult::Success();
        return StateBackendResult::Failure(
            std::move(result.message),
            result.integrity == BackendIntegrity::Unknown
                ? StateIntegrity::Unknown
                : StateIntegrity::Preserved);
    }
    catch (const std::exception& ex)
    {
        return StateBackendResult::Failure(
            ex.what(),
            StateIntegrity::Unknown);
    }
    catch (...)
    {
        return StateBackendResult::Failure(
            "Dolphin state backend operation threw",
            StateIntegrity::Unknown);
    }
}

} // namespace

SessionStateBackendAdapter::SessionStateBackendAdapter(
    IDolphinBackend& backend) noexcept
    : backend_(backend)
{
}

void SessionStateBackendAdapter::ConfigureOpen(
    BackendOpenOptions options)
{
    open_options_ = std::move(options);
    configured_ = true;
}

StateBackendResult SessionStateBackendAdapter::Boot(
    const StateBootRequest&)
{
    if (!configured_)
    {
        return StateBackendResult::Failure(
            "State backend has no session-open configuration");
    }
    return Invoke([&] { return backend_.Open(open_options_); });
}

StateBackendResult SessionStateBackendAdapter::Reboot(
    const StateBootRequest&)
{
    return Invoke([&] { return backend_.Reboot(); });
}

StateBackendResult SessionStateBackendAdapter::Shutdown() noexcept
{
    return Invoke([&] { return backend_.Close(); });
}

StateCompatibilityToken
SessionStateBackendAdapter::CurrentCompatibility() const
{
    return backend_.StateCompatibility();
}

StateBackendBufferResult
SessionStateBackendAdapter::SaveStateBuffer()
{
    try
    {
        BackendBufferResult result = backend_.SaveStateBuffer();
        if (result.result.ok)
        {
            return {
                StateBackendResult::Success(),
                std::move(result.bytes)};
        }
        return {
            StateBackendResult::Failure(
                std::move(result.result.message),
                result.result.integrity == BackendIntegrity::Unknown
                    ? StateIntegrity::Unknown
                    : StateIntegrity::Preserved),
            {}};
    }
    catch (const std::exception& ex)
    {
        return {
            StateBackendResult::Failure(
                ex.what(),
                StateIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            StateBackendResult::Failure(
                "Dolphin state-buffer capture threw",
                StateIntegrity::Unknown),
            {}};
    }
}

StateBackendResult SessionStateBackendAdapter::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    return Invoke([&] { return backend_.RestoreStateBuffer(bytes); });
}

StateBackendResult SessionStateBackendAdapter::SaveStateFile(
    const std::filesystem::path& path)
{
    return Invoke([&] { return backend_.SaveStateFile(path); });
}

StateBackendResult SessionStateBackendAdapter::RestoreStateFile(
    const std::filesystem::path& path)
{
    return Invoke([&] { return backend_.RestoreStateFile(path); });
}

StateBackendResult SessionStateBackendAdapter::Convert(
    BackendResult result) noexcept
{
    if (result.ok)
        return StateBackendResult::Success();
    return StateBackendResult::Failure(
        std::move(result.message),
        result.integrity == BackendIntegrity::Unknown
            ? StateIntegrity::Unknown
            : StateIntegrity::Preserved);
}

} // namespace savor::runtime
