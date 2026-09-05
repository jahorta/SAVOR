#include "WorkspaceStagingCleanup.h"

#include <set>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace savor::db {
namespace {

bool SafeRelativeDirectory(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory())
        return false;
    for (const auto& part : path)
    {
        if (part.empty() || part == "." || part == "..")
            return false;
    }
    return true;
}

bool IsReparsePoint(
    const std::filesystem::path& path,
    std::error_code* error_out)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return false;
        if (error_out)
            *error_out = std::error_code(
                static_cast<int>(code), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    const bool result = std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, error));
    if (error_out)
        *error_out = error;
    return result;
#endif
}

bool Inventory(
    const std::filesystem::path& directory,
    WorkspaceStagingCleanupSummary* summary,
    std::string* error_out)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error))
    {
        if (error)
        {
            if (error_out)
                *error_out = "failed inspecting staging directory '" +
                    directory.string() + "': " + error.message();
            return false;
        }
        return true;
    }
    if (!std::filesystem::is_directory(directory, error) || error)
    {
        if (error_out)
            *error_out = "staging path is not a directory: " +
                directory.string();
        return false;
    }
    if (IsReparsePoint(directory, &error) || error)
    {
        if (error_out)
            *error_out = "refusing to reset a reparse-point staging root: " +
                directory.string();
        return false;
    }

    std::filesystem::recursive_directory_iterator cursor(
        directory, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && cursor != end)
    {
        const auto path = cursor->path();
        if (IsReparsePoint(path, &error))
        {
            cursor.disable_recursion_pending();
            if (error_out)
                *error_out = "refusing to reset staging containing a reparse point: " +
                    path.string();
            return false;
        }
        if (error)
            break;
        const auto status = cursor->symlink_status(error);
        if (error)
            break;
        if (std::filesystem::is_directory(status))
        {
            ++summary->directories;
        }
        else
        {
            ++summary->files;
            if (std::filesystem::is_regular_file(status))
            {
                const auto size = cursor->file_size(error);
                if (error)
                    break;
                summary->bytes += size;
            }
        }
        cursor.increment(error);
    }
    if (error)
    {
        if (error_out)
            *error_out = "failed inventorying staging directory '" +
                directory.string() + "': " + error.message();
        return false;
    }
    return true;
}

} // namespace

bool ResetWorkspaceStagingDirectories(
    const std::filesystem::path& workspace_root,
    std::span<const std::filesystem::path> relative_directories,
    WorkspaceStagingCleanupSummary* summary_out,
    std::string* error_out)
{
    WorkspaceStagingCleanupSummary summary{};
    std::error_code error;
    const auto root = std::filesystem::absolute(workspace_root, error)
                          .lexically_normal();
    if (error || root.empty() || root == root.root_path())
    {
        if (error_out)
            *error_out = "refusing to reset staging under an invalid workspace root";
        return false;
    }

    std::set<std::filesystem::path> targets;
    for (const auto& requested : relative_directories)
    {
        const auto relative = requested.lexically_normal();
        if (!SafeRelativeDirectory(relative))
        {
            if (error_out)
                *error_out = "staging directory must be safely relative: " +
                    requested.string();
            return false;
        }
        const auto target = (root / relative).lexically_normal();
        if (target == root || target.lexically_relative(root) != relative)
        {
            if (error_out)
                *error_out = "staging directory escapes its workspace root: " +
                    requested.string();
            return false;
        }
        if (!targets.insert(target).second)
        {
            if (error_out)
                *error_out = "duplicate staging directory requested: " +
                    requested.string();
            return false;
        }
    }

    for (const auto& target : targets)
    {
        if (!Inventory(target, &summary, error_out))
            return false;
    }
    for (const auto& target : targets)
    {
        std::filesystem::remove_all(target, error);
        if (error)
        {
            if (error_out)
                *error_out = "failed clearing staging directory '" +
                    target.string() + "': " + error.message();
            return false;
        }
        std::filesystem::create_directories(target, error);
        if (error)
        {
            if (error_out)
                *error_out = "failed recreating staging directory '" +
                    target.string() + "': " + error.message();
            return false;
        }
    }

    if (summary_out)
        *summary_out = summary;
    if (error_out)
        error_out->clear();
    return true;
}

} // namespace savor::db
