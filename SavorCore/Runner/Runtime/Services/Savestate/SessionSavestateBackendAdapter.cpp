#include "Runner/Runtime/Services/Savestate/SessionSavestateBackendAdapter.h"

#include <exception>
#include <utility>

namespace savor::runtime {
namespace {

template <typename Operation>
SavestateBackendResult Invoke(Operation&& operation) noexcept
{
    try
    {
        BackendResult result = operation();
        if (result.ok)
            return SavestateBackendResult::Success();
        return SavestateBackendResult::Failure(
            std::move(result.message),
            result.integrity == BackendIntegrity::Unknown
                ? GuestIntegrity::Unknown
                : GuestIntegrity::Preserved);
    }
    catch (const std::exception& ex)
    {
        return SavestateBackendResult::Failure(
            ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        return SavestateBackendResult::Failure(
            "Dolphin state backend operation threw",
            GuestIntegrity::Unknown);
    }
}

} // namespace

SessionSavestateBackendAdapter::SessionSavestateBackendAdapter(
    IDolphinBackend& backend) noexcept
    : backend_(backend)
{
}

ArtifactCompatibilityToken
SessionSavestateBackendAdapter::CurrentCompatibility() const
{
    return backend_.SavestateCompatibility();
}

SavestateBackendBufferResult
SessionSavestateBackendAdapter::SaveStateBuffer()
{
    try
    {
        BackendBufferResult result = backend_.SaveStateBuffer();
        if (result.result.ok)
        {
            return {
                SavestateBackendResult::Success(),
                std::move(result.bytes)};
        }
        return {
            SavestateBackendResult::Failure(
                std::move(result.result.message),
                result.result.integrity == BackendIntegrity::Unknown
                    ? GuestIntegrity::Unknown
                    : GuestIntegrity::Preserved),
            {}};
    }
    catch (const std::exception& ex)
    {
        return {
            SavestateBackendResult::Failure(
                ex.what(),
                GuestIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            SavestateBackendResult::Failure(
                "Dolphin state-buffer capture threw",
                GuestIntegrity::Unknown),
            {}};
    }
}

SavestateBackendResult SessionSavestateBackendAdapter::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    return Invoke([&] { return backend_.RestoreStateBuffer(bytes); });
}

SavestateBackendResult SessionSavestateBackendAdapter::SaveStateFile(
    const std::filesystem::path& path)
{
    return Invoke([&] { return backend_.SaveStateFile(path); });
}

SavestateBackendResult SessionSavestateBackendAdapter::RestoreStateFile(
    const std::filesystem::path& path)
{
    return Invoke([&] { return backend_.RestoreStateFile(path); });
}

} // namespace savor::runtime
