#include "ActionViewStdJsonCache.h"

#include <Utils/Hash.h>
#include <Utils/IniDoc.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace savor::predict {
namespace {

constexpr std::uint32_t kManifestSchemaVersion = 1;
constexpr std::string_view kManifestResourceSet =
    "first-battle-soldiers-action-view-std-v1";
constexpr std::string_view kManifestSection = "action_view_std_manifest";
constexpr std::string_view kManifestFilesSection = "files";

struct ManifestVerification {
    bool present = false;
    bool verified = false;
    std::uint32_t schema_version = 0;
    std::vector<std::string> hash_mismatches;
    std::vector<std::string> diagnostics;
};

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool is_sha256(std::string_view value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

std::vector<std::string> missing_required_files(const std::filesystem::path& dir) {
    std::vector<std::string> missing;
    for (const auto& name : required_first_battle_action_view_std_json_files()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dir / name, ec)) {
            missing.push_back(name);
        }
    }
    return missing;
}

ManifestVerification verify_manifest(const std::filesystem::path& dir) {
    ManifestVerification result;
    const auto manifest_path = dir / action_view_std_json_manifest_filename();
    std::error_code ec;
    result.present = std::filesystem::is_regular_file(manifest_path, ec);
    if (!result.present) {
        result.diagnostics.push_back(
            "action-view STD JSON manifest is missing: " + manifest_path.string());
        return result;
    }

    const auto manifest = IniDoc::load(manifest_path.string());
    if (!manifest.has_value()) {
        result.diagnostics.push_back(
            "action-view STD JSON manifest could not be read: " + manifest_path.string());
        return result;
    }

    result.schema_version = manifest->get_u32(
        std::string(kManifestSection),
        "schema_version",
        0);
    if (result.schema_version != kManifestSchemaVersion) {
        result.diagnostics.push_back(
            "unsupported action-view STD JSON manifest schema_version="
            + std::to_string(result.schema_version));
        return result;
    }

    const auto resource_set = manifest->get(
        std::string(kManifestSection),
        "resource_set",
        "");
    if (resource_set != kManifestResourceSet) {
        result.diagnostics.push_back(
            "action-view STD JSON manifest resource_set mismatch: " + resource_set);
        return result;
    }

    const auto required_files = required_first_battle_action_view_std_json_files();
    const auto file_count = manifest->get_u32(
        std::string(kManifestSection),
        "file_count",
        0);
    if (file_count != required_files.size()) {
        result.diagnostics.push_back(
            "action-view STD JSON manifest file_count mismatch: expected "
            + std::to_string(required_files.size())
            + ", got " + std::to_string(file_count));
        return result;
    }

    for (const auto& name : required_files) {
        const auto expected = lowercase_ascii(manifest->get(
            std::string(kManifestFilesSection),
            name,
            ""));
        if (!is_sha256(expected)) {
            result.hash_mismatches.push_back(name + ":missing_or_invalid_manifest_hash");
            continue;
        }

        try {
            const auto actual = lowercase_ascii(hash::sha256_of_file((dir / name).string()));
            if (actual != expected) {
                result.hash_mismatches.push_back(name + ":sha256_mismatch");
            }
        } catch (const std::exception& error) {
            result.hash_mismatches.push_back(name + ":hash_failed");
            result.diagnostics.push_back(
                "failed to hash action-view STD JSON file " + name + ": " + error.what());
        }
    }

    if (!result.hash_mismatches.empty()) {
        result.diagnostics.push_back(
            "action-view STD JSON manifest verification failed for "
            + std::to_string(result.hash_mismatches.size()) + " file(s)");
        return result;
    }

    result.verified = true;
    return result;
}

void apply_manifest_verification(
    ActionViewStdJsonCacheResolution& result,
    const ManifestVerification& verification) {
    result.manifest_present = verification.present;
    result.manifest_verified = verification.verified;
    result.manifest_schema_version = verification.schema_version;
    result.hash_mismatches = verification.hash_mismatches;
    result.diagnostics.insert(
        result.diagnostics.end(),
        verification.diagnostics.begin(),
        verification.diagnostics.end());
}

void add_diag(ActionViewStdJsonCacheResolution& result, std::string text) {
    result.diagnostics.push_back(std::move(text));
}

} // namespace

std::vector<std::string> required_first_battle_action_view_std_json_files() {
    return {
        "ma000.std.json",
        "ma0000.std.json",
        "MA001.std.json",
        "ma0010.std.json",
        "MB000.std.json",
        "mb0000.std.json",
    };
}

std::string action_view_std_json_manifest_filename() {
    return "savor_action_view_std_manifest_v1.ini";
}

bool write_action_view_std_json_manifest(
    const std::filesystem::path& std_json_dir,
    std::string* error) {
    const auto missing = missing_required_files(std_json_dir);
    if (!missing.empty()) {
        if (error != nullptr) {
            *error = "cannot write action-view STD JSON manifest while required files are missing";
        }
        return false;
    }

    IniDoc manifest;
    manifest.ensure_section(std::string(kManifestSection));
    manifest.set(
        std::string(kManifestSection),
        "schema_version",
        std::to_string(kManifestSchemaVersion));
    manifest.set(
        std::string(kManifestSection),
        "resource_set",
        std::string(kManifestResourceSet));
    manifest.set(
        std::string(kManifestSection),
        "file_count",
        std::to_string(required_first_battle_action_view_std_json_files().size()));
    manifest.ensure_section(std::string(kManifestFilesSection));

    try {
        for (const auto& name : required_first_battle_action_view_std_json_files()) {
            manifest.set(
                std::string(kManifestFilesSection),
                name,
                lowercase_ascii(hash::sha256_of_file((std_json_dir / name).string())));
        }
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }

    const auto manifest_path = std_json_dir / action_view_std_json_manifest_filename();
    const auto temporary_path = manifest_path.string() + ".tmp";
    if (!manifest.save(temporary_path, true)) {
        if (error != nullptr) {
            *error = "failed to write temporary action-view STD JSON manifest";
        }
        return false;
    }

    std::string publish_error;
#if defined(_WIN32)
    if (!MoveFileExW(
            std::filesystem::path(temporary_path).c_str(),
            manifest_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        publish_error = "Win32 error " + std::to_string(GetLastError());
    }
#else
    std::error_code ec;
    std::filesystem::remove(manifest_path, ec);
    ec.clear();
    std::filesystem::rename(temporary_path, manifest_path, ec);
    if (ec) {
        publish_error = ec.message();
    }
#endif
    if (!publish_error.empty()) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        if (error != nullptr) {
            *error = "failed to publish action-view STD JSON manifest: " + publish_error;
        }
        return false;
    }
    return true;
}

std::filesystem::path default_action_view_std_json_cache_dir(const std::filesystem::path& db_root) {
    return db_root / ".std_json";
}

std::filesystem::path default_action_view_std_disc_dump_root() {
    return "D:/SoAGC/2002-12-19-gc-us-final_Skies_of_Arcadia_Legends";
}

ActionViewStdJsonCacheResolution resolve_action_view_std_json_cache(
    const ActionViewStdJsonCacheOptions& options) {
    ActionViewStdJsonCacheResolution result;
    if (options.explicit_std_json_dir.empty()) {
        add_diag(
            result,
            "implicit action-view STD JSON cache lookup and generation are disabled; "
            "provide --action-view-std-json-dir for explicit legacy JSON compatibility");
        return result;
    }

    result.used_explicit_dir = true;
    result.resolved_std_json_dir = options.explicit_std_json_dir;
    result.manifest_path =
        result.resolved_std_json_dir / action_view_std_json_manifest_filename();
    result.missing_files_before = missing_required_files(result.resolved_std_json_dir);
    if (!result.missing_files_before.empty()) {
        result.missing_files_after = result.missing_files_before;
        result.fatal_error = true;
        add_diag(
            result,
            "explicit --action-view-std-json-dir is missing required first-battle files");
        return result;
    }

    apply_manifest_verification(result, verify_manifest(result.resolved_std_json_dir));
    if (!result.manifest_verified) {
        result.fatal_error = true;
        add_diag(
            result,
            "explicit --action-view-std-json-dir failed content-manifest verification");
        return result;
    }

    result.cache_complete_before = true;
    result.available = true;
    return result;
}

std::string summarize_action_view_std_json_cache_resolution(
    const ActionViewStdJsonCacheResolution& resolution) {
    std::ostringstream out;
    out << "std_json_dir="
        << (resolution.resolved_std_json_dir.empty()
            ? std::string("none")
            : resolution.resolved_std_json_dir.string())
        << "; available=" << (resolution.available ? "true" : "false")
        << "; manifest_verified=" << (resolution.manifest_verified ? "true" : "false")
        << "; manifest_schema_version=" << resolution.manifest_schema_version
        << "; generation_attempted=" << (resolution.generation_attempted ? "true" : "false")
        << "; generation_succeeded=" << (resolution.generation_succeeded ? "true" : "false");
    if (!resolution.missing_files_after.empty()) {
        out << "; missing=";
        for (std::size_t i = 0; i < resolution.missing_files_after.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << resolution.missing_files_after[i];
        }
    }
    if (!resolution.hash_mismatches.empty()) {
        out << "; hash_mismatches=";
        for (std::size_t i = 0; i < resolution.hash_mismatches.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << resolution.hash_mismatches[i];
        }
    }
    return out.str();
}

} // namespace savor::predict
