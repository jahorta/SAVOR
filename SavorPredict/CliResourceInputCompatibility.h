#pragma once

#include "ActionViewStdJsonCache.h"
#include "SpiceBattleContentAdapter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace savor::predict::cli_detail {

inline void add_unique_diagnostic(
    std::vector<std::string>& diagnostics,
    std::string diagnostic) {
    if (std::find(diagnostics.begin(), diagnostics.end(), diagnostic)
        == diagnostics.end()) {
        diagnostics.push_back(std::move(diagnostic));
    }
}

inline std::string normalized_path_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    auto key = (ec ? path : absolute).lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

struct DiscDumpRootOptions {
    std::optional<std::filesystem::path> canonical;
    std::optional<std::filesystem::path> deprecated_alias;

    void observe(
        std::string_view option,
        const std::filesystem::path& value,
        std::vector<std::string>& errors) {
        auto& destination = option == "--disc-dump-root"
            ? canonical
            : deprecated_alias;
        if (destination.has_value()
            && normalized_path_key(*destination) != normalized_path_key(value)) {
            add_unique_diagnostic(
                errors,
                std::string(option)
                    + " was specified multiple times with conflicting values.");
            return;
        }
        destination = value;
    }

    void finalize(
        std::filesystem::path& destination,
        std::vector<std::string>& errors,
        std::vector<std::string>& warnings) const {
        if (deprecated_alias.has_value()) {
            add_unique_diagnostic(
                warnings,
                "--std-disc-dump-root is deprecated; use --disc-dump-root.");
        }
        if (canonical.has_value() && deprecated_alias.has_value()
            && normalized_path_key(*canonical)
                != normalized_path_key(*deprecated_alias)) {
            add_unique_diagnostic(
                errors,
                "--disc-dump-root and --std-disc-dump-root must resolve "
                "to the same path when both are specified.");
            return;
        }
        if (canonical.has_value()) {
            destination = *canonical;
        } else if (deprecated_alias.has_value()) {
            destination = *deprecated_alias;
        }
    }
};

inline void add_spice_file_parsing_exe_warning(
    std::vector<std::string>& warnings) {
    add_unique_diagnostic(
        warnings,
        "--spice-file-parsing-exe is deprecated and ignored by direct "
        "SPICE input mode.");
}

inline const char* resource_diagnostic_severity_name(
    BattlePredictorResourceDiagnosticSeverity severity) {
    switch (severity) {
    case BattlePredictorResourceDiagnosticSeverity::Info:
        return "info";
    case BattlePredictorResourceDiagnosticSeverity::Warning:
        return "warning";
    case BattlePredictorResourceDiagnosticSeverity::Error:
        return "error";
    }
    return "error";
}

inline void write_resource_input_diagnostics(
    const SpiceBattleContentAdapterResult& result,
    std::ostream& err) {
    err << "Resource inputs: status="
        << battle_predictor_resource_input_status_name(result.status)
        << "\n";
    for (const auto& diagnostic : result.diagnostics) {
        err << "Resource inputs: "
            << resource_diagnostic_severity_name(diagnostic.severity);
        if (!diagnostic.logical_role.empty()) {
            err << " [" << diagnostic.logical_role << "]";
        }
        err << ": " << diagnostic.message;
        if (diagnostic.source_offset.has_value()) {
            err << " offset=" << *diagnostic.source_offset;
        }
        err << "\n";
    }
}

inline bool ensure_first_battle_resource_inputs(
    std::filesystem::path& disc_dump_root,
    const std::filesystem::path& legacy_std_json_dir,
    BattlePredictorResourceBundlePtr& resource_inputs,
    std::ostream& err) {
    if (resource_inputs != nullptr) {
        if (resource_inputs->status
            == BattlePredictorResourceInputStatus::Ready) {
            return true;
        }
        err << "Resource inputs: supplied bundle has status="
            << battle_predictor_resource_input_status_name(
                resource_inputs->status)
            << "\n";
        return false;
    }

    if (disc_dump_root.empty()) {
        disc_dump_root = default_action_view_std_disc_dump_root();
    }
    const auto result = load_first_battle_spice_resource_bundle({
        .disc_dump_root = disc_dump_root,
        .legacy_std_json_dir = legacy_std_json_dir,
        .spice_revision = {},
    });
    write_resource_input_diagnostics(result, err);
    if (!result.ok()) {
        return false;
    }
    resource_inputs = result.bundle;
    return true;
}

} // namespace savor::predict::cli_detail
