#include "ActionViewStdJsonCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace savor::predict {
namespace {

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
        result.available = true;
        result.missing_files_before = missing_required_files(result.resolved_std_json_dir);
        if (!result.missing_files_before.empty()) {
            add_diag(result, "explicit --action-view-std-json-dir is missing first-battle files; downstream loader may fall back or report missing data");
        }
        return result;
    }

    result.missing_files_before = missing_required_files(result.cache_dir);
    result.cache_complete_before = result.missing_files_before.empty();
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
    return out.str();
}

} // namespace savor::predict
