#include "BattleJobRunManifest.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace savor::predict {
namespace {

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            const auto uch = static_cast<unsigned char>(ch);
            if (uch < 0x20) {
                out << "\\u00";
                constexpr char kHex[] = "0123456789abcdef";
                out << kHex[(uch >> 4) & 0x0f] << kHex[uch & 0x0f];
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string path_string(const std::filesystem::path& path) {
    return path.empty() ? std::string{} : path.string();
}

void write_json_string_array(std::ostream& out, const char* name, const std::vector<std::string>& values, bool comma) {
    out << "  \"" << name << "\": [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(values[i]) << "\"";
    }
    out << "]";
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void write_table_counts(std::ostream& out, const std::vector<savor::dbutils::TableCopyCount>& values) {
    out << "  \"sandbox_table_counts\": [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "{\"db\":\"" << json_escape(values[i].db_name)
            << "\",\"table\":\"" << json_escape(values[i].table_name)
            << "\",\"rows\":" << values[i].rows_copied << "}";
    }
    out << "],\n";
}

void write_copied_artifacts(std::ostream& out, const std::vector<savor::dbutils::CopiedArtifactFile>& values) {
    out << "  \"sandbox_copied_artifacts\": [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "{\"artifact_id\":" << values[i].artifact_id
            << ",\"savestate_id\":" << values[i].savestate_id
            << ",\"source_path\":\"" << json_escape(path_string(values[i].source_path))
            << "\",\"copied_path\":\"" << json_escape(path_string(values[i].copied_path))
            << "\"}";
    }
    out << "],\n";
}

std::string hex_u32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

void write_optional_hex_u32(std::ostream& out, const char* name, const std::optional<std::uint32_t>& value, bool comma) {
    out << "  \"" << name << "\": ";
    if (value.has_value()) {
        out << "\"" << hex_u32(*value) << "\"";
    } else {
        out << "null";
    }
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void write_optional_bool(std::ostream& out, const char* name, const std::optional<bool>& value, bool comma) {
    out << "  \"" << name << "\": ";
    if (value.has_value()) {
        out << (*value ? "true" : "false");
    } else {
        out << "null";
    }
    if (comma) {
        out << ",";
    }
    out << "\n";
}

} // namespace

bool write_battle_job_run_manifest(
    const BattleJobRunSummary& summary,
    const std::filesystem::path& manifest_path,
    std::ostream& err) {
    if (manifest_path.empty()) {
        err << "Manifest path is empty.\n";
        return false;
    }
    if (const auto parent = manifest_path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err << "Failed creating manifest directory: " << ec.message() << "\n";
            return false;
        }
    }
    std::ofstream file(manifest_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed opening manifest: " << manifest_path.string() << "\n";
        return false;
    }

    file << "{\n";
    file << "  \"source_db_root\": \"" << json_escape(path_string(summary.options.db_root)) << "\",\n";
    file << "  \"run_root\": \"" << json_escape(path_string(summary.sandbox.run_root)) << "\",\n";
    file << "  \"sandbox_db_root\": \"" << json_escape(path_string(summary.sandbox.db_root)) << "\",\n";
    file << "  \"sandbox_mode\": \"" << json_escape(savor::dbutils::ToString(summary.sandbox.sandbox_mode)) << "\",\n";
    file << "  \"iso_path\": \"" << json_escape(path_string(summary.options.iso_path)) << "\",\n";
    file << "  \"dolphin_base_dir\": \"" << json_escape(path_string(summary.options.dolphin_base_dir)) << "\",\n";
    file << "  \"worker_exe_path\": \"" << json_escape(path_string(summary.options.worker_exe_path)) << "\",\n";
    file << "  \"capture_profile_path\": \"" << json_escape(path_string(summary.capture_profile_path)) << "\",\n";
    file << "  \"expected_capture_path\": \"" << json_escape(path_string(summary.expected_capture_path)) << "\",\n";
    file << "  \"stable_capture_path\": \"" << json_escape(path_string(summary.stable_capture_path)) << "\",\n";
    file << "  \"trace_report_path\": \"" << json_escape(path_string(summary.trace_report_path)) << "\",\n";
    file << "  \"original_turn_job_id\": " << summary.clone.original_turn_job_id << ",\n";
    file << "  \"original_exec_job_id\": " << summary.clone.original_exec_job_id << ",\n";
    file << "  \"cloned_turn_job_id\": " << summary.clone.cloned_turn_job_id << ",\n";
    file << "  \"cloned_exec_job_id\": " << summary.clone.cloned_exec_job_id << ",\n";
    file << "  \"original_job_set_id\": " << summary.clone.original_job_set_id << ",\n";
    file << "  \"battle_set_id\": " << summary.clone.battle_set_id << ",\n";
    file << "  \"wave_id\": " << summary.clone.wave_id << ",\n";
    file << "  \"turn_index\": " << summary.clone.turn_index << ",\n";
    file << "  \"fake_attacks_this_turn\": " << summary.clone.fake_attacks_this_turn << ",\n";
    write_optional_hex_u32(file, "override_start_rng_seed", summary.options.override_start_rng_seed, true);
    write_optional_hex_u32(file, "captured_original_seed", summary.captured_original_seed, true);
    write_optional_hex_u32(file, "captured_override_seed", summary.captured_override_seed, true);
    write_optional_hex_u32(file, "captured_applied_seed", summary.captured_applied_seed, true);
    write_optional_bool(file, "captured_seed_readback_matches", summary.captured_seed_readback_matches, true);
    file << "  \"quarantined_ready_jobs\": " << summary.clone.quarantined_ready_jobs << ",\n";
    file << "  \"terminal_state\": \"" << json_escape(summary.terminal_state) << "\",\n";
    file << "  \"timed_out\": " << (summary.timed_out ? "true" : "false") << ",\n";
    file << "  \"capture_found\": " << (summary.capture_found ? "true" : "false") << ",\n";
    file << "  \"trace_exit_code\": " << summary.trace_exit_code << ",\n";
    write_table_counts(file, summary.sandbox.table_counts);
    write_copied_artifacts(file, summary.sandbox.copied_artifacts);
    write_json_string_array(file, "sandbox_validation_errors", summary.sandbox.validation_errors, true);
    write_json_string_array(file, "events", summary.events, true);
    write_json_string_array(file, "errors", summary.errors, false);
    file << "}\n";
    return file.good();
}

bool write_battle_job_run_text_summary(
    const BattleJobRunSummary& summary,
    const std::filesystem::path& summary_path,
    std::ostream& err) {
    if (summary_path.empty()) {
        err << "Summary path is empty.\n";
        return false;
    }
    std::ofstream file(summary_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed opening summary: " << summary_path.string() << "\n";
        return false;
    }
    file << "SavorPredict run-battle-job\n";
    file << "source_db_root: " << summary.options.db_root.string() << "\n";
    file << "sandbox_db_root: " << summary.sandbox.db_root.string() << "\n";
    file << "sandbox_mode: " << savor::dbutils::ToString(summary.sandbox.sandbox_mode) << "\n";
    file << "original_exec_job_id: " << summary.clone.original_exec_job_id << "\n";
    file << "cloned_exec_job_id: " << summary.clone.cloned_exec_job_id << "\n";
    file << "original_turn_job_id: " << summary.clone.original_turn_job_id << "\n";
    file << "cloned_turn_job_id: " << summary.clone.cloned_turn_job_id << "\n";
    file << "override_start_rng_seed: "
        << (summary.options.override_start_rng_seed.has_value() ? hex_u32(*summary.options.override_start_rng_seed) : "none")
        << "\n";
    if (summary.captured_original_seed.has_value()) {
        file << "captured_original_seed: " << hex_u32(*summary.captured_original_seed) << "\n";
        file << "captured_override_seed: " << hex_u32(summary.captured_override_seed.value_or(0)) << "\n";
        file << "captured_applied_seed: " << hex_u32(summary.captured_applied_seed.value_or(0)) << "\n";
        file << "captured_seed_readback_matches: "
            << (summary.captured_seed_readback_matches.value_or(false) ? "true" : "false") << "\n";
    }
    file << "terminal_state: " << summary.terminal_state << "\n";
    file << "capture_found: " << (summary.capture_found ? "true" : "false") << "\n";
    file << "stable_capture_path: " << summary.stable_capture_path.string() << "\n";
    file << "trace_report_path: " << summary.trace_report_path.string() << "\n";
    for (const auto& error : summary.errors) {
        file << "error: " << error << "\n";
    }
    return file.good();
}

} // namespace savor::predict
