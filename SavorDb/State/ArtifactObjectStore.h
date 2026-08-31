#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace savor::db::state {

struct ManagedArtifactObject {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
};

std::optional<std::filesystem::path> MakeArtifactObjectRelativePath(
    std::string_view expected_sha256,
    std::string_view file_ext,
    std::string* error_out = nullptr);

// State artifact locators are always relative to DbConfigPaths::object_store_root.
// They are never interpreted relative to the process working directory.
bool IsValidArtifactObjectRelativePath(
    const std::filesystem::path& relative_path,
    std::string* error_out = nullptr);

std::optional<std::filesystem::path> ResolveArtifactObjectPath(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& relative_path,
    std::string* error_out = nullptr);

std::optional<std::filesystem::path> ResolveWorkspaceArtifactSource(
    const std::filesystem::path& artifact_workspace_root,
    const std::filesystem::path& workspace_relative_path,
    std::string* error_out = nullptr);

// Publishes a verified absolute source into the canonical State artifact
// namespace and returns the portable locator persisted in object_relpath.
std::optional<ManagedArtifactObject> PublishVerifiedArtifactObject(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& absolute_source_path,
    std::string_view expected_sha256,
    std::int64_t expected_size_bytes,
    std::string_view file_ext,
    std::string* error_out = nullptr);

} // namespace savor::db::state
