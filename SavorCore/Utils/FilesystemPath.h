#pragma once

#include <filesystem>
#include <string>

namespace savor::filesystem {

[[nodiscard]] inline bool ResolveNativeIoPath(
    const std::filesystem::path& path,
    std::filesystem::path* resolved_out,
    std::string* error_out = nullptr)
{
    if (resolved_out == nullptr)
    {
        if (error_out) *error_out = "Native I/O path output is required";
        return false;
    }

    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
    {
        if (error_out)
            *error_out = "Unable to resolve absolute filesystem path: " +
                error.message();
        return false;
    }
    absolute = absolute.lexically_normal();

#ifdef _WIN32
    const auto native = absolute.native();
    if (native.rfind(LR"(\\?\)", 0) == 0)
    {
        *resolved_out = std::move(absolute);
        return true;
    }
    if (native.rfind(LR"(\\)", 0) == 0)
    {
        *resolved_out = std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)") + native.substr(2));
        return true;
    }
    if (native.size() < 3 || native[1] != L':' ||
        (native[2] != L'\\' && native[2] != L'/'))
    {
        if (error_out)
            *error_out = "Windows I/O path is not drive-qualified";
        return false;
    }
    *resolved_out = std::filesystem::path(
        std::wstring(LR"(\\?\)") + native);
#else
    *resolved_out = std::move(absolute);
#endif
    return true;
}

} // namespace savor::filesystem
