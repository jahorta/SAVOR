#include "ArtifactObjectStore.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "../../SavorCore/Utils/Hash.h"

namespace savor::db::state {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string NormalizeExtension(std::string_view file_ext) {
    if (file_ext.empty()) return {};
    std::string value(file_ext);
    if (value.front() != '.') value.insert(value.begin(), '.');
    return value;
}

bool ExactFile(
    const std::filesystem::path& path,
    std::int64_t expected_size_bytes,
    std::string* error_out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        if (error_out) *error_out = "artifact object is not a regular file: " + path.string();
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || expected_size_bytes < 0 ||
        size != static_cast<std::uintmax_t>(expected_size_bytes)) {
        if (error_out) *error_out = "artifact object size does not match durable evidence: " + path.string();
        return false;
    }
    return true;
}

bool IsCanonicalSha256(std::string_view value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::optional<std::filesystem::path> ObjectStoreSuffix(
    const std::filesystem::path& recorded_path) {
    bool found = false;
    std::filesystem::path suffix;
    for (const auto& component : recorded_path) {
        if (!found) {
            if (Lower(component.string()) == "object_store") found = true;
            continue;
        }
        suffix /= component;
    }
    if (!found || suffix.empty()) return std::nullopt;
    return suffix;
}

} // namespace

std::optional<std::filesystem::path> MakeArtifactObjectRelativePath(
    std::string_view expected_sha256,
    std::string_view file_ext,
    std::string* error_out) {
    if (expected_sha256.empty()) {
        if (error_out) *error_out = "artifact SHA-256 identity is empty";
        return std::nullopt;
    }
    const auto extension = NormalizeExtension(file_ext);
    const auto object_key = IsCanonicalSha256(expected_sha256)
        ? Lower(std::string(expected_sha256))
        : hash::sha256(expected_sha256.data(), expected_sha256.size());
    return std::filesystem::path("state_artifacts") /
        object_key.substr(0, 2) /
        object_key.substr(2, 2) /
        (object_key + extension);
}

bool IsValidArtifactObjectRelativePath(
    const std::filesystem::path& relative_path,
    std::string* error_out) {
    if (relative_path.empty() || relative_path.is_absolute() ||
        relative_path.has_root_name() || relative_path.has_root_directory()) {
        if (error_out) *error_out = "artifact object locator must be a non-empty relative path";
        return false;
    }
    for (const auto& component : relative_path) {
        const auto value = component.string();
        if (value.empty() || value == "." || value == "..") {
            if (error_out) *error_out = "artifact object locator contains an invalid path component";
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> ResolveArtifactObjectPath(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& relative_path,
    std::string* error_out) {
    if (object_store_root.empty()) {
        if (error_out) *error_out = "artifact object-store root is empty";
        return std::nullopt;
    }
    if (!IsValidArtifactObjectRelativePath(relative_path, error_out)) {
        return std::nullopt;
    }
    return (std::filesystem::absolute(object_store_root) / relative_path).lexically_normal();
}

std::optional<std::filesystem::path> ResolveLegacyArtifactSource(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& recorded_path,
    std::string* error_out) {
    if (recorded_path.empty()) {
        if (error_out) *error_out = "legacy artifact locator is empty";
        return std::nullopt;
    }
    std::error_code ec;
    if (recorded_path.is_absolute() && std::filesystem::is_regular_file(recorded_path, ec) && !ec) {
        return recorded_path;
    }
    if (!recorded_path.is_absolute()) {
        auto resolved = ResolveArtifactObjectPath(object_store_root, recorded_path, error_out);
        if (resolved && std::filesystem::is_regular_file(*resolved, ec) && !ec) return resolved;
    }
    if (auto suffix = ObjectStoreSuffix(recorded_path)) {
        auto rebound = ResolveArtifactObjectPath(object_store_root, *suffix, error_out);
        if (rebound && std::filesystem::is_regular_file(*rebound, ec) && !ec) return rebound;
    }
    if (error_out) {
        *error_out = "artifact source file does not exist at its recorded or relocated object-store path: " +
            recorded_path.string();
    }
    return std::nullopt;
}

std::optional<ManagedArtifactObject> ImportArtifactObject(
    const std::filesystem::path& object_store_root,
    const std::filesystem::path& source_path,
    std::string_view expected_sha256,
    std::int64_t expected_size_bytes,
    std::string_view file_ext,
    std::string* error_out) {
    if (expected_sha256.empty() || expected_size_bytes < 0) {
        if (error_out) *error_out = "invalid artifact identity";
        return std::nullopt;
    }
    auto source = ResolveLegacyArtifactSource(object_store_root, source_path, error_out);
    if (!source || !ExactFile(*source, expected_size_bytes, error_out)) {
        return std::nullopt;
    }

    const auto relative = MakeArtifactObjectRelativePath(
        expected_sha256, file_ext, error_out);
    if (!relative) return std::nullopt;
    auto destination = ResolveArtifactObjectPath(
        object_store_root, *relative, error_out);
    if (!destination) return std::nullopt;

    std::error_code ec;
    if (std::filesystem::exists(*destination, ec) && !ec) {
        if (!ExactFile(*destination, expected_size_bytes, error_out)) {
            return std::nullopt;
        }
        return ManagedArtifactObject{relative->generic_string(), *destination};
    }

    std::filesystem::create_directories(destination->parent_path(), ec);
    if (ec) {
        if (error_out) *error_out = "failed creating artifact object directory: " + ec.message();
        return std::nullopt;
    }
    const auto temporary = destination->parent_path() /
        (destination->filename().string() + ".importing");
    std::filesystem::copy_file(
        *source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error_out) *error_out = "failed copying artifact into object store: " + ec.message();
        return std::nullopt;
    }
    if (!ExactFile(temporary, expected_size_bytes, error_out)) {
        std::filesystem::remove(temporary, ec);
        return std::nullopt;
    }
    std::filesystem::rename(temporary, *destination, ec);
    if (ec) {
        if (std::filesystem::exists(*destination) &&
            ExactFile(*destination, expected_size_bytes, error_out)) {
            std::filesystem::remove(temporary, ec);
        } else {
            if (error_out) *error_out = "failed publishing artifact object: " + ec.message();
            std::filesystem::remove(temporary, ec);
            return std::nullopt;
        }
    }
    return ManagedArtifactObject{relative->generic_string(), *destination};
}

} // namespace savor::db::state
