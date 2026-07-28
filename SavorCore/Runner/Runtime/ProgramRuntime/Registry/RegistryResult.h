#pragma once

#include <cstdint>
#include <string>

namespace savor::runtime::program {

enum class RegistryErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidIdentity,
    IdentityConflict,
    DependencyMissing,
    DependencyCycle,
    ReferenceMismatch,
    CompatibilityMismatch,
    NotFound,
};

struct RegistryError
{
    RegistryErrorCode code = RegistryErrorCode::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != RegistryErrorCode::None;
    }
};

struct RegistryResult
{
    bool success = false;
    RegistryError error;

    [[nodiscard]] static RegistryResult Success() noexcept
    {
        return {true, {}};
    }

    [[nodiscard]] static RegistryResult Failure(
        RegistryErrorCode code,
        std::string message)
    {
        return {false, {code, std::move(message)}};
    }
};

} // namespace savor::runtime::program
