#include "BattleJobBatchRunManifest.h"

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

std::string csv_escape(const std::string& value) {
    bool needs_quotes = false;
    for (const char ch : value) {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
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

void write_std_json_cache(std::ostream& out, const ActionViewStdJsonCacheResolution& value) {
    out << "  \"std_json_cache\": {"
        << "\"resolved_std_json_dir\":\"" << json_escape(path_string(value.resolved_std_json_dir)) << "\","
        << "\"cache_dir\":\"" << json_escape(path_string(value.cache_dir)) << "\","
        << "\"disc_dump_root\":\"" << json_escape(path_string(value.disc_dump_root)) << "\","
        << "\"spice_file_parsing_exe\":\"" << json_escape(path_string(value.spice_file_parsing_exe)) << "\","
        << "\"used_explicit_dir\":" << (value.used_explicit_dir ? "true" : "false") << ","
        << "\"cache_complete_before\":" << (value.cache_complete_before ? "true" : "false") << ","
        << "\"generation_attempted\":" << (value.generation_attempted ? "true" : "false") << ","
        << "\"generation_succeeded\":" << (value.generation_succeeded ? "true" : "false") << ","
        << "\"available\":" << (value.available ? "true" : "false") << ","
        << "\"fatal_error\":" << (value.fatal_error ? "true" : "false") << ","
        << "\"spice_exit_code\":" << value.spice_exit_code
        << "},\n";
}

void ensure_parent_dir(const std::filesystem::path& path, std::ostream& err, bool* ok) {
    if (!*ok) {
        return;
    }
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err << "Failed creating output directory " << parent.string() << ": " << ec.message() << "\n";
            *ok = false;
        }
    }
}

std::string hex_u32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

void write_optional_hex_u32(std::ostream& out, const char* indent, const char* name, const std::optional<std::uint32_t>& value, bool comma) {
    out << indent << "\"" << name << "\": ";
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

void write_optional_u32(std::ostream& out, const char* indent, const char* name, const std::optional<std::uint32_t>& value, bool comma) {
    out << indent << "\"" << name << "\": ";
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
    if (comma) {
        out << ",";
    }
    out << "\n";
}

void write_optional_bool(std::ostream& out, const char* indent, const char* name, const std::optional<bool>& value, bool comma) {
    out << indent << "\"" << name << "\": ";
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

std::string optional_hex_for_text(const std::optional<std::uint32_t>& value) {
    return value.has_value() ? hex_u32(*value) : "none";
}

std::string optional_u32_for_text(const std::optional<std::uint32_t>& value) {
    return value.has_value() ? std::to_string(*value) : "none";
}

void write_requested_runs(std::ostream& out, const BattleJobBatchRunOptions& options) {
    const auto requests = resolved_battle_job_batch_requests(options);
    out << "  \"requested_runs\": [";
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "{\"exec_job_id\":" << requests[i].exec_job_id
            << ",\"override_start_rng_seed\":";
        if (requests[i].override_start_rng_seed.has_value()) {
            out << "\"" << hex_u32(*requests[i].override_start_rng_seed) << "\"";
        } else {
            out << "null";
        }
        out << ",\"override_fake_attacks_this_turn\":";
        if (requests[i].override_fake_attacks_this_turn.has_value()) {
            out << *requests[i].override_fake_attacks_this_turn;
        } else {
            out << "null";
        }
        out << ",\"battle_run_ms\":";
        if (requests[i].battle_run_ms.has_value()) {
            out << *requests[i].battle_run_ms;
        } else {
            out << "null";
        }
        out << "}";
    }
    out << "],\n";
}

void write_capture_artifact(
    std::ostream& out,
    const char* indent,
    const PreparedCaptureArtifact& value,
    bool comma)
{
    out << indent << "\"capture_artifact\": {"
        << "\"verified\":" << (value.verified ? "true" : "false") << ","
        << "\"complete\":" << (value.complete ? "true" : "false") << ","
        << "\"incomplete_reason\":\"" << json_escape(value.incomplete_reason) << "\","
        << "\"segment_count\":" << value.segment_count << ","
        << "\"chunk_count\":" << value.chunk_count << ","
        << "\"event_count\":" << value.event_count << ","
        << "\"gap_count\":" << value.gap_count << ","
        << "\"revision_count\":" << value.revision_count << ","
        << "\"metrics_event_count\":" << value.metrics_event_count << ","
        << "\"hits\":" << value.hits << ","
        << "\"filter_rejections\":" << value.filter_rejections << ","
        << "\"capture_deliveries\":" << value.capture_deliveries << ","
        << "\"progress_deliveries\":" << value.progress_deliveries << ","
        << "\"control_publications\":" << value.control_publications << ","
        << "\"drops\":" << value.drops << ","
        << "\"progress_coalesced\":" << value.progress_coalesced << ","
        << "\"bytes\":" << value.bytes << ","
        << "\"traces\":" << value.traces << ","
        << "\"trace_failures\":" << value.trace_failures << ","
        << "\"flight_triggers\":" << value.flight_triggers << ","
        << "\"flight_completed_windows\":" << value.flight_completed_windows << ","
        << "\"flight_window_active\":" << (value.flight_window_active ? "true" : "false") << ","
        << "\"segments\":[";
    for (std::size_t i = 0; i < value.segments.size(); ++i) {
        if (i != 0) out << ',';
        out << '"' << json_escape(path_string(value.segments[i])) << '"';
    }
    out << "]}" << (comma ? "," : "") << "\n";
}

} // namespace

bool write_battle_job_batch_run_manifest(
    const BattleJobBatchRunSummary& summary,
    const std::filesystem::path& manifest_path,
    std::ostream& err) {
    if (manifest_path.empty()) {
        err << "Manifest path is empty.\n";
        return false;
    }
    bool ok = true;
    ensure_parent_dir(manifest_path, err, &ok);
    if (!ok) {
        return false;
    }

    std::ofstream file(manifest_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed opening manifest: " << manifest_path.string() << "\n";
        return false;
    }

    file << "{\n";
    file << "  \"command\": \"run-battle-jobs\",\n";
    file << "  \"source_db_root\": \"" << json_escape(path_string(summary.options.db_root)) << "\",\n";
    file << "  \"run_root\": \"" << json_escape(path_string(summary.sandbox.run_root)) << "\",\n";
    file << "  \"sandbox_db_root\": \"" << json_escape(path_string(summary.sandbox.db_root)) << "\",\n";
    file << "  \"sandbox_mode\": \"" << json_escape(savor::dbutils::ToString(summary.sandbox.sandbox_mode)) << "\",\n";
    file << "  \"iso_path\": \"" << json_escape(path_string(summary.options.iso_path)) << "\",\n";
    file << "  \"dolphin_base_dir\": \"" << json_escape(path_string(summary.options.dolphin_base_dir)) << "\",\n";
    file << "  \"worker_exe_path\": \"" << json_escape(path_string(summary.options.worker_exe_path)) << "\",\n";
    file << "  \"probe_mode\": \"" << probe_mode_name(summary.options.probe_mode) << "\",\n";
    file << "  \"probe_cpu_core\": \"" << probe_cpu_core_name(summary.options.probe_cpu_core) << "\",\n";
    file << "  \"capture_profile_path\": \"" << json_escape(path_string(summary.capture_profile_path)) << "\",\n";
    write_std_json_cache(file, summary.std_json_cache);
    file << "  \"worker_count\": " << summary.worker_count << ",\n";
    file << "  \"max_workers\": " << summary.options.max_workers << ",\n";
    file << "  \"wait_for_workers_ready\": "
        << (summary.options.wait_for_workers_ready ? "true" : "false") << ",\n";
    file << "  \"poll_ms\": " << summary.options.poll_ms << ",\n";
    file << "  \"timeout_ms\": " << summary.timeout_ms << ",\n";
    write_optional_u32(file, "  ", "battle_run_ms", summary.options.battle_run_ms, true);
    write_optional_hex_u32(file, "  ", "override_start_rng_seed", summary.options.override_start_rng_seed, true);
    write_optional_u32(file, "  ", "override_fake_attacks_this_turn", summary.options.override_fake_attacks_this_turn, true);
    file << "  \"timed_out\": " << (summary.timed_out ? "true" : "false") << ",\n";

    const auto source_exec_job_ids = unique_battle_job_batch_source_exec_job_ids(summary.options);
    file << "  \"source_exec_job_ids\": [";
    for (std::size_t i = 0; i < source_exec_job_ids.size(); ++i) {
        if (i != 0) {
            file << ", ";
        }
        file << source_exec_job_ids[i];
    }
    file << "],\n";
    write_requested_runs(file, summary.options);

    file << "  \"quarantined_ready_jobs\": " << summary.clone.quarantined_ready_jobs << ",\n";
    write_table_counts(file, summary.sandbox.table_counts);
    write_copied_artifacts(file, summary.sandbox.copied_artifacts);
    write_json_string_array(file, "sandbox_validation_errors", summary.sandbox.validation_errors, true);

    file << "  \"jobs\": [\n";
    for (std::size_t i = 0; i < summary.jobs.size(); ++i) {
        const auto& job = summary.jobs[i];
        file << "    {\n";
        file << "      \"original_turn_job_id\": " << job.clone.original_turn_job_id << ",\n";
        file << "      \"original_exec_job_id\": " << job.clone.original_exec_job_id << ",\n";
        file << "      \"cloned_turn_job_id\": " << job.clone.cloned_turn_job_id << ",\n";
        file << "      \"cloned_exec_job_id\": " << job.clone.cloned_exec_job_id << ",\n";
        file << "      \"original_job_set_id\": " << job.clone.original_job_set_id << ",\n";
        file << "      \"battle_set_id\": " << job.clone.battle_set_id << ",\n";
        file << "      \"wave_id\": " << job.clone.wave_id << ",\n";
        file << "      \"turn_index\": " << job.clone.turn_index << ",\n";
        file << "      \"source_fake_attacks_this_turn\": " << job.clone.source_fake_attacks_this_turn << ",\n";
        file << "      \"fake_attacks_this_turn\": " << job.clone.fake_attacks_this_turn << ",\n";
        write_optional_u32(file, "      ", "battle_run_ms", job.clone.battle_run_ms, true);
        write_optional_hex_u32(file, "      ", "override_start_rng_seed", job.clone.override_start_rng_seed, true);
        write_optional_u32(file, "      ", "override_fake_attacks_this_turn", job.clone.override_fake_attacks_this_turn, true);
        write_optional_hex_u32(file, "      ", "captured_original_seed", job.captured_original_seed, true);
        write_optional_hex_u32(file, "      ", "captured_override_seed", job.captured_override_seed, true);
        write_optional_hex_u32(file, "      ", "captured_applied_seed", job.captured_applied_seed, true);
        write_optional_bool(file, "      ", "captured_seed_readback_matches", job.captured_seed_readback_matches, true);
        file << "      \"terminal_state\": \"" << json_escape(job.terminal_state) << "\",\n";
        file << "      \"timed_out\": " << (job.timed_out ? "true" : "false") << ",\n";
        file << "      \"capture_found\": " << (job.capture_found ? "true" : "false") << ",\n";
        write_capture_artifact(file, "      ", job.capture_artifact, true);
        file << "      \"trace_exit_code\": " << job.trace_exit_code << ",\n";
        file << "      \"expected_capture_path\": \"" << json_escape(path_string(job.expected_capture_path)) << "\",\n";
        file << "      \"stable_capture_path\": \"" << json_escape(path_string(job.stable_capture_path)) << "\",\n";
        file << "      \"capture_export_path\": \"" << json_escape(path_string(job.capture_export_path)) << "\",\n";
        file << "      \"trace_report_path\": \"" << json_escape(path_string(job.trace_report_path)) << "\",\n";
        file << "      \"errors\": [";
        for (std::size_t e = 0; e < job.errors.size(); ++e) {
            if (e != 0) {
                file << ", ";
            }
            file << "\"" << json_escape(job.errors[e]) << "\"";
        }
        file << "]\n";
        file << "    }" << (i + 1 == summary.jobs.size() ? "" : ",") << "\n";
    }
    file << "  ],\n";

    write_json_string_array(file, "events", summary.events, true);
    write_json_string_array(file, "errors", summary.errors, false);
    file << "}\n";
    return file.good();
}

bool write_battle_job_batch_run_text_summary(
    const BattleJobBatchRunSummary& summary,
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
    file << "SavorPredict run-battle-jobs\n";
    file << "source_db_root: " << summary.options.db_root.string() << "\n";
    file << "sandbox_db_root: " << summary.sandbox.db_root.string() << "\n";
    file << "sandbox_mode: " << savor::dbutils::ToString(summary.sandbox.sandbox_mode) << "\n";
    file << "probe_mode: " << probe_mode_name(summary.options.probe_mode) << "\n";
    file << "probe_cpu_core: " << probe_cpu_core_name(summary.options.probe_cpu_core) << "\n";
    file << "worker_count: " << summary.worker_count << "\n";
    file << "wait_for_workers_ready: "
        << (summary.options.wait_for_workers_ready ? "true" : "false") << "\n";
    file << "timeout_ms: " << summary.timeout_ms << "\n";
    file << "battle_run_ms: " << optional_u32_for_text(summary.options.battle_run_ms) << "\n";
    file << "override_start_rng_seed: " << optional_hex_for_text(summary.options.override_start_rng_seed) << "\n";
    file << "override_fake_attacks_this_turn: " << optional_u32_for_text(summary.options.override_fake_attacks_this_turn) << "\n";
    file << "std_json_cache: "
        << summarize_action_view_std_json_cache_resolution(summary.std_json_cache) << "\n";
    file << "timed_out: " << (summary.timed_out ? "true" : "false") << "\n";
    file << "jobs: " << summary.jobs.size() << "\n";
    for (const auto& job : summary.jobs) {
        file << "job original_exec=" << job.clone.original_exec_job_id
            << " cloned_exec=" << job.clone.cloned_exec_job_id
            << " state=" << (job.terminal_state.empty() ? "unknown" : job.terminal_state)
            << " source_fake_attacks=" << job.clone.source_fake_attacks_this_turn
            << " fake_attacks=" << job.clone.fake_attacks_this_turn
            << " battle_run_ms=" << optional_u32_for_text(job.clone.battle_run_ms)
            << " override=" << optional_hex_for_text(job.clone.override_start_rng_seed)
            << " override_fake_attacks=" << optional_u32_for_text(job.clone.override_fake_attacks_this_turn)
            << " original_seed=" << optional_hex_for_text(job.captured_original_seed)
            << " applied_seed=" << optional_hex_for_text(job.captured_applied_seed)
            << " capture=" << (job.capture_found
                ? job.stable_capture_path.string()
                : (summary.options.probe_mode == ProbeMode::Capture ? "missing" : "disabled"))
            << " segments=" << job.capture_artifact.segment_count
            << " chunks=" << job.capture_artifact.chunk_count
            << " complete=" << (job.capture_artifact.complete ? "true" : "false")
            << " gaps=" << job.capture_artifact.gap_count
            << " drops=" << job.capture_artifact.drops
            << " revisions=" << job.capture_artifact.revision_count
            << " trace=" << job.trace_report_path.string()
            << " errors=" << job.errors.size() << "\n";
        for (const auto& error : job.errors) {
            file << "  error: " << error << "\n";
        }
    }
    for (const auto& error : summary.errors) {
        file << "error: " << error << "\n";
    }
    return file.good();
}

bool write_battle_job_batch_run_csv_summary(
    const BattleJobBatchRunSummary& summary,
    const std::filesystem::path& csv_path,
    std::ostream& err) {
    if (csv_path.empty()) {
        err << "CSV summary path is empty.\n";
        return false;
    }
    std::ofstream file(csv_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed opening CSV summary: " << csv_path.string() << "\n";
        return false;
    }
    file << "original_exec_job_id,cloned_exec_job_id,original_turn_job_id,cloned_turn_job_id,"
        << "source_fake_attacks_this_turn,fake_attacks_this_turn,battle_run_ms,override_start_rng_seed,override_fake_attacks_this_turn,captured_original_seed,captured_override_seed,captured_applied_seed,"
        << "captured_seed_readback_matches,terminal_state,timed_out,probe_mode,capture_found,capture_complete,capture_segments,capture_chunks,capture_gaps,capture_drops,capture_revisions,trace_exit_code,stable_capture_path,capture_export_path,trace_report_path,error_count\n";
    for (const auto& job : summary.jobs) {
        file << job.clone.original_exec_job_id << ","
            << job.clone.cloned_exec_job_id << ","
            << job.clone.original_turn_job_id << ","
            << job.clone.cloned_turn_job_id << ","
            << job.clone.source_fake_attacks_this_turn << ","
            << job.clone.fake_attacks_this_turn << ","
            << csv_escape(optional_u32_for_text(job.clone.battle_run_ms)) << ","
            << csv_escape(optional_hex_for_text(job.clone.override_start_rng_seed)) << ","
            << csv_escape(optional_u32_for_text(job.clone.override_fake_attacks_this_turn)) << ","
            << csv_escape(optional_hex_for_text(job.captured_original_seed)) << ","
            << csv_escape(optional_hex_for_text(job.captured_override_seed)) << ","
            << csv_escape(optional_hex_for_text(job.captured_applied_seed)) << ","
            << (job.captured_seed_readback_matches.has_value()
                ? (*job.captured_seed_readback_matches ? "true" : "false")
                : "none") << ","
            << csv_escape(job.terminal_state) << ","
            << (job.timed_out ? "true" : "false") << ","
            << probe_mode_name(summary.options.probe_mode) << ","
            << (job.capture_found ? "true" : "false") << ","
            << (job.capture_artifact.complete ? "true" : "false") << ","
            << job.capture_artifact.segment_count << ","
            << job.capture_artifact.chunk_count << ","
            << job.capture_artifact.gap_count << ","
            << job.capture_artifact.drops << ","
            << job.capture_artifact.revision_count << ","
            << job.trace_exit_code << ","
            << csv_escape(path_string(job.stable_capture_path)) << ","
            << csv_escape(path_string(job.capture_export_path)) << ","
            << csv_escape(path_string(job.trace_report_path)) << ","
            << job.errors.size() << "\n";
    }
    return file.good();
}

} // namespace savor::predict
