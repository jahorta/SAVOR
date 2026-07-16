#include "ActionViewStdJsonCache.h"

#include <Utils/Hash.h>
#include <Utils/IniDoc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

std::string quote_windows_arg(const std::filesystem::path& path) {
    const auto value = path.string();
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    out += "\"";
    return out;
}

std::vector<std::filesystem::path> split_path_env() {
    std::vector<std::filesystem::path> paths;
#if defined(_WIN32)
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "PATH") != 0 || raw == nullptr) {
        return paths;
    }
    std::string owned(raw);
    std::free(raw);
    std::size_t start = 0;
    while (start <= owned.size()) {
        const auto semi = owned.find(';', start);
        const auto part = owned.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        if (!part.empty()) {
            paths.emplace_back(part);
        }
        if (semi == std::string::npos) {
            break;
        }
        start = semi + 1;
    }
#endif
    return paths;
}

std::filesystem::path first_existing_file(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path discover_spice_file_parsing_exe() {
    std::vector<std::filesystem::path> candidates;
    const auto cwd = std::filesystem::current_path();
    candidates.push_back(cwd / ".." / "SPICE" / "bin" / "x64" / "Debug" / "SpiceFileParsing.exe");
    candidates.push_back(cwd / ".." / ".." / ".." / ".." / "SPICE" / "bin" / "x64" / "Debug" / "SpiceFileParsing.exe");
    for (const auto& path_dir : split_path_env()) {
        candidates.push_back(path_dir / "SpiceFileParsing.exe");
    }
    return first_existing_file(candidates);
}

SpiceStdJsonExportResult run_spice_file_parsing_process(const SpiceStdJsonExportRequest& request) {
    SpiceStdJsonExportResult result;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::string command =
        quote_windows_arg(request.spice_file_parsing_exe)
        + " "
        + quote_windows_arg(request.bchara_dir)
        + " "
        + quote_windows_arg(request.output_dir)
        + " --export-std-json";

    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    const BOOL created = CreateProcessA(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    CloseHandle(write_pipe);
    write_pipe = nullptr;

    if (!created) {
        CloseHandle(read_pipe);
        result.error = "CreateProcessA failed for " + request.spice_file_parsing_exe.string();
        return result;
    }

    std::thread reader([&]() {
        char buffer[4096];
        DWORD bytes_read = 0;
        while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
            result.output.append(buffer, buffer + bytes_read);
        }
    });

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }

    reader.join();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    return result;
#else
    (void)request;
    result.error = "SPICE export process execution is only implemented on Windows";
    return result;
#endif
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

std::filesystem::path default_spice_file_parsing_exe() {
    return discover_spice_file_parsing_exe();
}

ActionViewStdJsonCacheResolution resolve_action_view_std_json_cache(
    const ActionViewStdJsonCacheOptions& options,
    SpiceStdJsonExportRunner runner) {
    ActionViewStdJsonCacheResolution result;
    result.cache_dir = default_action_view_std_json_cache_dir(options.db_root);
    result.disc_dump_root = options.std_disc_dump_root.empty()
        ? default_action_view_std_disc_dump_root()
        : options.std_disc_dump_root;
    result.spice_file_parsing_exe = options.spice_file_parsing_exe.empty()
        ? default_spice_file_parsing_exe()
        : options.spice_file_parsing_exe;

    if (!options.explicit_std_json_dir.empty()) {
        result.used_explicit_dir = true;
        result.resolved_std_json_dir = options.explicit_std_json_dir;
        result.manifest_path =
            result.resolved_std_json_dir / action_view_std_json_manifest_filename();
        result.missing_files_before = missing_required_files(result.resolved_std_json_dir);
        if (!result.missing_files_before.empty()) {
            result.fatal_error = true;
            add_diag(result, "explicit --action-view-std-json-dir is missing required first-battle files");
            return result;
        }
        apply_manifest_verification(result, verify_manifest(result.resolved_std_json_dir));
        if (!result.manifest_verified) {
            result.fatal_error = true;
            add_diag(result, "explicit --action-view-std-json-dir failed content-manifest verification");
            return result;
        }
        result.cache_complete_before = true;
        result.available = true;
        return result;
    }

    result.manifest_path = result.cache_dir / action_view_std_json_manifest_filename();
    result.missing_files_before = missing_required_files(result.cache_dir);
    if (result.missing_files_before.empty()) {
        apply_manifest_verification(result, verify_manifest(result.cache_dir));
    }
    result.cache_complete_before =
        result.missing_files_before.empty() && result.manifest_verified;
    if (result.cache_complete_before) {
        result.available = true;
        result.resolved_std_json_dir = result.cache_dir;
        add_diag(result, "action-view STD JSON cache hit");
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(result.disc_dump_root, ec)) {
        add_diag(result, "action-view STD JSON cache incomplete and disc dump root is unavailable: "
            + result.disc_dump_root.string());
        result.missing_files_after = result.missing_files_before;
        return result;
    }

    const auto bchara_dir = result.disc_dump_root / "bchara";
    if (!std::filesystem::is_directory(bchara_dir, ec)) {
        result.fatal_error = true;
        add_diag(result, "disc dump root exists but does not contain bchara: " + bchara_dir.string());
        result.missing_files_after = result.missing_files_before;
        return result;
    }

    if (result.spice_file_parsing_exe.empty()
        || !std::filesystem::is_regular_file(result.spice_file_parsing_exe, ec)) {
        add_diag(result, "action-view STD JSON cache incomplete and SpiceFileParsing.exe was not found");
        result.missing_files_after = result.missing_files_before;
        return result;
    }

    std::filesystem::create_directories(result.cache_dir, ec);
    if (ec) {
        add_diag(result, "action-view STD JSON cache incomplete and cache directory could not be created: " + ec.message());
        result.missing_files_after = result.missing_files_before;
        return result;
    }

    result.generation_attempted = true;
    const SpiceStdJsonExportRequest request{
        .spice_file_parsing_exe = result.spice_file_parsing_exe,
        .bchara_dir = bchara_dir,
        .output_dir = result.cache_dir,
    };
    const auto export_result = runner ? runner(request) : run_spice_file_parsing_process(request);
    result.spice_exit_code = export_result.exit_code;
    result.spice_output = export_result.output;
    if (!export_result.error.empty()) {
        add_diag(result, export_result.error);
    }
    if (export_result.exit_code != 0) {
        result.fatal_error = true;
        add_diag(result, "SpiceFileParsing --export-std-json failed with exit code "
            + std::to_string(export_result.exit_code));
        result.missing_files_after = missing_required_files(result.cache_dir);
        return result;
    }

    result.missing_files_after = missing_required_files(result.cache_dir);
    if (!result.missing_files_after.empty()) {
        result.fatal_error = true;
        add_diag(result, "SpiceFileParsing completed but required first-battle STD JSON files are still missing");
        return result;
    }

    std::string manifest_error;
    if (!write_action_view_std_json_manifest(result.cache_dir, &manifest_error)) {
        result.fatal_error = true;
        add_diag(result, "SpiceFileParsing completed but the content manifest could not be written: "
            + manifest_error);
        return result;
    }
    result.manifest_written = true;
    apply_manifest_verification(result, verify_manifest(result.cache_dir));
    if (!result.manifest_verified) {
        result.fatal_error = true;
        add_diag(result, "generated action-view STD JSON cache failed content-manifest verification");
        return result;
    }

    result.generation_succeeded = true;
    result.available = true;
    result.resolved_std_json_dir = result.cache_dir;
    add_diag(result, "action-view STD JSON cache generated from disc dump");
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
