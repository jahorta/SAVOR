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

// Imports an exact source file into the canonical State artifact namespace and
// returns the portable locator that must be persisted in
// state_artifact.object_relpath.
std::optional<ManagedArtifactObject> ImportArtifactObject(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& source_path,
    std::string_view expected_sha256,
    std::int64_t expected_size_bytes,
    std::string_view file_ext,
    std::string* error_out = nullptr);

// Resolves a pre-hard-cut locator. Absolute paths under an earlier object-store
// root are rebound by their suffix when the workspace has been relocated.
std::optional<std::filesystem::path> ResolveLegacyArtifactSource(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& recorded_path,
    std::string* error_out = nullptr);

} // namespace savor::db::state
