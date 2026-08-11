#include "BattlePredictionBatchRun.h"

#include "BattlePredictionScenario.h"
#include "DbCopy.h"
#include "Utils/Hash.h"

#include <picojson.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <ostream>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace savor::predict {
namespace {

struct ResolvedBatchOptions {
    BattlePredictionBatchRunOptions options;
    std::vector<std::string> errors;
};

struct PreflightJob {
    BattlePredictionBatchJobSummary summary;
    std::optional<BattlePredictionDbInput> input;
};

std::string json_escape(std::string_view value) {
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
                constexpr char kHex[] = "0123456789abcdef";
                out << "\\u00"
                    << kHex[(uch >> 4) & 0x0f]
                    << kHex[uch & 0x0f];
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string csv_escape(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(value);
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << separator;
        }
        out << values[i];
    }
    return out.str();
}

std::string job_filename(long long source_exec_job_id, std::string_view extension) {
    return "job_" + std::to_string(source_exec_job_id) + std::string(extension);
}

bool looks_like_json_object(std::string_view value) {
    picojson::value parsed;
    auto position = value.begin();
    const auto parse_error =
        picojson::parse(parsed, position, value.end());
    return parse_error.empty()
        && parsed.is<picojson::value::object>();
}

bool write_file(
    const std::filesystem::path& path,
    std::string_view contents,
    std::ostream& err) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed opening batch artifact " << path.string() << ".\n";
        return false;
    }
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    if (!file.good()) {
        err << "Failed writing batch artifact " << path.string() << ".\n";
        return false;
    }
    return true;
}

void write_json_string_array(
    std::ostream& out,
    const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(values[i]) << "\"";
    }
    out << "]";
}

void write_json_i64_array(
    std::ostream& out,
    const std::vector<long long>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << values[i];
    }
    out << "]";
}

void write_optional_string(
    std::ostream& out,
    const std::optional<std::string>& value) {
    if (value.has_value()) {
        out << "\"" << json_escape(*value) << "\"";
    } else {
        out << "null";
    }
}

void write_optional_i64(
    std::ostream& out,
    const std::optional<long long>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void write_optional_int(
    std::ostream& out,
    const std::optional<int>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void write_optional_u32(
    std::ostream& out,
    const std::optional<std::uint32_t>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

ResolvedBatchOptions resolve_options(
    const BattlePredictionBatchRunOptions& supplied) {
    ResolvedBatchOptions resolved{.options = supplied};
    if (resolved.options.scenario_name.has_value()) {
        const auto scenario =
            battle_prediction_scenario_by_name(*resolved.options.scenario_name);
        if (!scenario.has_value()) {
            resolved.errors.push_back(
                "Unsupported scenario: " + *resolved.options.scenario_name);
            return resolved;
        }
        resolved.options.scenario_name = scenario->name;
        if (resolved.options.profile_name.empty()) {
            resolved.options.profile_name = scenario->profile_name;
        } else if (resolved.options.profile_name != scenario->profile_name
            && !resolved.options.allow_profile_overrides) {
            resolved.errors.push_back(
                "Scenario " + scenario->name + " requires profile "
                + scenario->profile_name + ".");
        }
        if (!resolved.options.source_selection.has_value()) {
            resolved.options.source_selection = scenario->source_selection;
        }
    }
    return resolved;
}

bool create_run_directories(
    const std::filesystem::path& run_root,
    std::ostream& err) {
    std::error_code ec;
    if (std::filesystem::exists(run_root, ec)) {
        if (ec) {
            err << "Failed inspecting batch run root " << run_root.string()
                << ": " << ec.message() << "\n";
            return false;
        }
        if (!std::filesystem::is_directory(run_root, ec) || ec) {
            err << "Batch run root is not a directory: "
                << run_root.string() << "\n";
            return false;
        }
        if (!std::filesystem::is_empty(run_root, ec) || ec) {
            err << "Batch run root must be new or empty: "
                << run_root.string() << "\n";
            return false;
        }
    }
    std::filesystem::create_directories(run_root / "predictions", ec);
    if (ec) {
        err << "Failed creating predictions directory: "
            << ec.message() << "\n";
        return false;
    }
    std::filesystem::create_directories(run_root / "stderr", ec);
    if (ec) {
        err << "Failed creating stderr directory: "
            << ec.message() << "\n";
        return false;
    }
    return true;
}

bool uses_legacy_std_json(
    const BattlePredictorResourceBundlePtr& resource_inputs) {
    return resource_inputs != nullptr
        && resource_inputs->provider_kind
            == BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld;
}

void write_resource_inputs_json(
    std::ostream& out,
    const BattlePredictorResourceBundlePtr& resource_inputs) {
    if (resource_inputs == nullptr) {
        out << "null";
        return;
    }
    out << "{"
        << "\"provider_kind\":\""
        << battle_predictor_resource_provider_kind_name(
            resource_inputs->provider_kind)
        << "\",\"status\":\""
        << battle_predictor_resource_input_status_name(
            resource_inputs->status)
        << "\",\"adapter_version\":\""
        << json_escape(resource_inputs->adapter_version)
        << "\",\"spice_revision\":\""
        << json_escape(resource_inputs->spice_revision)
        << "\",\"bundle_digest\":\""
        << json_escape(resource_inputs->bundle_digest)
        << "\",\"sources\":[";
    for (std::size_t i = 0; i < resource_inputs->sources.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        const auto& source = resource_inputs->sources[i];
        out << "{\"logical_role\":\""
            << json_escape(source.logical_role)
            << "\",\"relative_path\":\""
            << json_escape(source.relative_path)
            << "\",\"normalized_relative_path\":\""
            << json_escape(source.normalized_relative_path)
            << "\",\"source_path\":\""
            << json_escape(source.source_path)
            << "\",\"size_bytes\":" << source.size_bytes
            << ",\"sha256\":\"" << json_escape(source.sha256)
            << "\",\"parser_identity\":\""
            << json_escape(source.parser_identity)
            << "\",\"parser_status\":\""
            << json_escape(source.parser_status) << "\"}";
    }
    out << "],\"diagnostic_count\":"
        << resource_inputs->diagnostics.size() << "}";
}

std::string request_json(
    const BattlePredictionBatchRunOptions& options,
    std::string_view started_at_utc) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 2,\n";
    out << "  \"requested_at_utc\": \"" << json_escape(started_at_utc) << "\",\n";
    out << "  \"run_name\": \"" << json_escape(options.run_name) << "\",\n";
    out << "  \"run_root\": \""
        << json_escape(options.run_root.generic_string()) << "\",\n";
    out << "  \"db_root\": \""
        << json_escape(options.db_root.generic_string()) << "\",\n";
    out << "  \"profile\": \"" << json_escape(options.profile_name) << "\",\n";
    out << "  \"scenario\": ";
    write_optional_string(out, options.scenario_name);
    out << ",\n";
    out << "  \"resource_inputs\": ";
    write_resource_inputs_json(out, options.resource_inputs);
    out << ",\n";
    if (uses_legacy_std_json(options.resource_inputs)) {
        out << "  \"action_view_std_json_dir\": \""
            << json_escape(
                options.action_view_std_json_dir.generic_string())
            << "\",\n";
    }
    out << "  \"allow_seed_candidate_fallback\": "
        << (options.allow_seed_candidate_fallback ? "true" : "false") << ",\n";
    out << "  \"allow_profile_overrides\": "
        << (options.allow_profile_overrides ? "true" : "false") << ",\n";
    out << "  \"emit_causal_diagnostics\": "
        << (options.emit_causal_diagnostics ? "true" : "false") << ",\n";
    out << "  \"require_complete\": "
        << (options.require_complete ? "true" : "false") << ",\n";
    out << "  \"preflight_only\": "
        << (options.preflight_only ? "true" : "false") << ",\n";
    out << "  \"source_exec_job_ids\": ";
    write_json_i64_array(out, options.source_exec_job_ids);
    out << "\n}\n";
    return out.str();
}

void add_partial_reason(
    std::vector<std::string>& reasons,
    std::string reason) {
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(std::move(reason));
    }
}

std::vector<std::string> prediction_partial_reasons(
    const BattlePredictionResult& prediction) {
    std::vector<std::string> reasons;
    if (prediction.has_missing_input_events) {
        add_partial_reason(reasons, "missing_input_events");
    }
    if (prediction.has_unsupported_events) {
        add_partial_reason(reasons, "unsupported_events");
    }
    if (prediction.has_ambiguous_events) {
        add_partial_reason(reasons, "ambiguous_events");
    }
    if (prediction.has_provisional_events) {
        add_partial_reason(reasons, "provisional_events");
    }
    switch (prediction.outcome) {
    case BattlePredictionOutcome::ReachedNextTurn:
    case BattlePredictionOutcome::Victory:
    case BattlePredictionOutcome::Defeat:
        break;
    case BattlePredictionOutcome::MissingInput:
    case BattlePredictionOutcome::Unsupported:
    case BattlePredictionOutcome::Ambiguous:
    case BattlePredictionOutcome::Provisional:
        add_partial_reason(
            reasons,
            "outcome=" + std::string(
                battle_prediction_outcome_name(prediction.outcome)));
        break;
    }
    for (const auto& error : prediction.errors) {
        add_partial_reason(reasons, "prediction_error=" + error);
    }
    return reasons;
}

int single_job_exit_code(const BattlePredictionResult& prediction) {
    return prediction.has_missing_input_events
        || prediction.has_unsupported_events
        || prediction.has_ambiguous_events
        || !prediction.errors.empty() ? 1 : 0;
}

std::string file_fingerprint_json(const std::filesystem::path& path) {
    std::ostringstream out;
    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(path, ec) && !ec;
    out << "{\"path\":\"" << json_escape(path.generic_string())
        << "\",\"exists\":" << (exists ? "true" : "false");
    if (exists) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) {
            out << ",\"size\":" << size;
        }
        ec.clear();
        const auto modified = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            out << ",\"last_write_ticks\":"
                << modified.time_since_epoch().count();
        }
    }
    out << "}";
    return out.str();
}

std::optional<std::string> json_object_string(
    const picojson::value::object& object,
    std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->second.is<std::string>()) {
        return std::nullopt;
    }
    return it->second.get<std::string>();
}

std::optional<std::uintmax_t> json_object_size(
    const picojson::value::object& object,
    std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->second.is<double>()) {
        return std::nullopt;
    }
    const double value = it->second.get<double>();
    if (value < 0.0
        || value > static_cast<double>(
            std::numeric_limits<std::uintmax_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uintmax_t>(value);
}

bool reject_nonempty_wal(
    const std::filesystem::path& database_path,
    std::ostream& err) {
    const auto wal_path =
        std::filesystem::path(database_path.string() + "-wal");
    std::error_code ec;
    const bool wal_exists = std::filesystem::exists(wal_path, ec);
    if (ec) {
        err << "Failed inspecting SQLite WAL sidecar "
            << wal_path.string() << ": " << ec.message() << "\n";
        return false;
    }
    if (!wal_exists) {
        return true;
    }
    if (!std::filesystem::is_regular_file(wal_path, ec)) {
        if (ec) {
            err << "Failed inspecting SQLite WAL sidecar "
                << wal_path.string() << ": " << ec.message() << "\n";
            return false;
        }
        err << "SQLite WAL sidecar is not a regular file: "
            << wal_path.string() << "\n";
        return false;
    }
    const auto size = std::filesystem::file_size(wal_path, ec);
    if (ec) {
        err << "Failed reading SQLite WAL sidecar size "
            << wal_path.string() << ": " << ec.message() << "\n";
        return false;
    }
    if (size != 0) {
        err << "Prediction DB is not sealed; non-empty SQLite WAL sidecar: "
            << wal_path.string() << "\n";
        return false;
    }
    return true;
}

bool load_database_identity(
    const std::filesystem::path& db_root,
    BattlePredictionBatchDatabaseIdentity* identity,
    std::ostream& err) {
    const auto analysis_path = db_root / "analysis.db";
    const auto state_path = db_root / "state.db";
    if (!reject_nonempty_wal(analysis_path, err)
        || !reject_nonempty_wal(state_path, err)) {
        return false;
    }

    std::error_code size_error;
    identity->analysis_db_size =
        std::filesystem::file_size(analysis_path, size_error);
    if (size_error) {
        err << "Failed reading analysis.db size: "
            << size_error.message() << "\n";
        return false;
    }
    identity->state_db_size =
        std::filesystem::file_size(state_path, size_error);
    if (size_error) {
        err << "Failed reading state.db size: "
            << size_error.message() << "\n";
        return false;
    }
    if (!compute_file_sha256_streaming(
            analysis_path,
            &identity->analysis_db_sha256,
            err)
        || !compute_file_sha256_streaming(
            state_path,
            &identity->state_db_sha256,
            err)) {
        return false;
    }
    const std::string canonical_identity =
        "analysis.db\n"
        + std::to_string(identity->analysis_db_size) + "\n"
        + identity->analysis_db_sha256 + "\n"
        + "state.db\n"
        + std::to_string(identity->state_db_size) + "\n"
        + identity->state_db_sha256 + "\n";
    identity->used_database_fingerprint = hash::sha256(
        canonical_identity.data(), canonical_identity.size());
    if (identity->used_database_fingerprint.empty()) {
        err << "Failed computing used-database fingerprint.\n";
        return false;
    }

    const auto manifest_path = db_root / "db_manifest.json";
    std::error_code manifest_error;
    const bool manifest_exists =
        std::filesystem::exists(manifest_path, manifest_error);
    if (manifest_error) {
        err << "Failed inspecting prediction DB snapshot manifest: "
            << manifest_error.message() << "\n";
        return false;
    }
    identity->snapshot_manifest_present = manifest_exists;
    if (!identity->snapshot_manifest_present) {
        return true;
    }
    if (!std::filesystem::is_regular_file(
            manifest_path,
            manifest_error)
        || manifest_error) {
        err << "Prediction DB snapshot manifest is not a regular file: "
            << manifest_path.string() << "\n";
        return false;
    }
    if (!compute_file_sha256_streaming(
            manifest_path,
            &identity->snapshot_manifest_sha256,
            err)) {
        return false;
    }
    const auto manifest_size =
        std::filesystem::file_size(manifest_path, manifest_error);
    if (manifest_error || manifest_size > 8 * 1024 * 1024) {
        err << "Prediction DB snapshot manifest is unreadable or too large: "
            << manifest_path.string() << "\n";
        return false;
    }
    std::ifstream manifest_file(manifest_path, std::ios::binary);
    const std::string manifest{
        std::istreambuf_iterator<char>(manifest_file),
        std::istreambuf_iterator<char>()};
    if (!manifest_file && !manifest_file.eof()) {
        err << "Failed reading prediction DB snapshot manifest: "
            << manifest_path.string() << "\n";
        return false;
    }
    picojson::value parsed;
    auto position = manifest.begin();
    const auto parse_error =
        picojson::parse(parsed, position, manifest.end());
    if (!parse_error.empty()
        || !parsed.is<picojson::value::object>()) {
        err << "Prediction DB snapshot manifest is invalid JSON: "
            << parse_error << "\n";
        return false;
    }
    const auto& root = parsed.get<picojson::value::object>();
    if (json_object_string(root, "kind").value_or("")
        != "prediction_snapshot") {
        err << "Prediction DB snapshot manifest has an unsupported kind.\n";
        return false;
    }
    identity->snapshot_database_fingerprint =
        json_object_string(root, "database_fingerprint").value_or("");
    if (identity->snapshot_database_fingerprint.empty()) {
        err << "Prediction DB snapshot manifest has no database_fingerprint.\n";
        return false;
    }
    const auto databases_it = root.find("databases");
    if (databases_it == root.end()
        || !databases_it->second.is<picojson::value::array>()) {
        err << "Prediction DB snapshot manifest has no databases array.\n";
        return false;
    }

    const auto& manifest_databases =
        databases_it->second.get<picojson::value::array>();
    constexpr std::array<std::string_view, 6> kExpectedDatabaseNames{
        "analysis.db",
        "execution.db",
        "state.db",
        "ui_read.db",
        "authoring.db",
        "archive.db",
    };
    if (manifest_databases.size() != kExpectedDatabaseNames.size()) {
        err << "Prediction DB snapshot manifest does not contain the "
               "complete database set.\n";
        return false;
    }

    bool analysis_verified = false;
    bool state_verified = false;
    std::string manifest_canonical_identity;
    for (std::size_t database_index = 0;
         database_index < manifest_databases.size();
         ++database_index) {
        const auto& database_value =
            manifest_databases[database_index];
        if (!database_value.is<picojson::value::object>()) {
            err << "Prediction DB snapshot manifest contains an invalid "
                   "database entry.\n";
            return false;
        }
        const auto& database =
            database_value.get<picojson::value::object>();
        const auto name = json_object_string(database, "name").value_or("");
        const auto sha256 =
            json_object_string(database, "sha256").value_or("");
        const auto size = json_object_size(database, "size_bytes");
        if (name.empty() || sha256.empty() || !size.has_value()) {
            err << "Prediction DB snapshot manifest contains an incomplete "
                   "database fingerprint entry.\n";
            return false;
        }
        if (name != kExpectedDatabaseNames[database_index]) {
            err << "Prediction DB snapshot manifest database order or name "
                   "is invalid at index "
                << database_index << ".\n";
            return false;
        }
        manifest_canonical_identity +=
            name + "\n" + std::to_string(*size) + "\n" + sha256 + "\n";
        if (name != "analysis.db" && name != "state.db") {
            continue;
        }
        const bool matches = size.has_value()
            && (name == "analysis.db"
                ? *size == identity->analysis_db_size
                    && sha256 == identity->analysis_db_sha256
                : *size == identity->state_db_size
                    && sha256 == identity->state_db_sha256);
        if (!matches) {
            err << "Prediction DB snapshot manifest fingerprint mismatch for "
                << name << ".\n";
            return false;
        }
        if (name == "analysis.db") {
            analysis_verified = true;
        } else {
            state_verified = true;
        }
    }
    const auto computed_snapshot_fingerprint = hash::sha256(
        manifest_canonical_identity.data(),
        manifest_canonical_identity.size());
    if (computed_snapshot_fingerprint
        != identity->snapshot_database_fingerprint) {
        err << "Prediction DB snapshot manifest database_fingerprint "
               "does not match its database entries.\n";
        return false;
    }
    if (!analysis_verified || !state_verified) {
        err << "Prediction DB snapshot manifest does not identify both "
               "analysis.db and state.db.\n";
        return false;
    }
    identity->snapshot_manifest_verified = true;
    return true;
}

std::string executable_fingerprint_json(
    const std::filesystem::path& path) {
    std::ostringstream out;
    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(path, ec) && !ec;
    out << "{\"path\":\"" << json_escape(path.generic_string())
        << "\",\"exists\":" << (exists ? "true" : "false");
    if (exists) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) {
            out << ",\"size\":" << size;
        }
        try {
            const auto sha256 = hash::sha256_of_file(path.string());
            if (!sha256.empty()) {
                out << ",\"sha256\":\"" << sha256 << "\"";
            }
        } catch (const std::exception&) {
        }
    }
    out << "}";
    return out.str();
}

std::string summary_csv(
    const std::vector<BattlePredictionBatchJobSummary>& jobs) {
    std::ostringstream out;
    out << "source_exec_job_id,invocation_status,prediction_status,"
           "prediction_outcome,json_valid,single_job_exit_code,turn_job_id,"
           "resolved_exec_job_id,wave_id,battle_set_id,entry_savestate_id,"
           "seed_candidate_id,starting_rng_seed,seed_source,final_rng_seed,"
           "total_draws_consumed,partial_reasons,diagnostics\n";
    for (const auto& job : jobs) {
        out << job.source_exec_job_id << ","
            << battle_prediction_batch_invocation_status_name(
                job.invocation_status) << ","
            << battle_prediction_batch_prediction_status_name(
                job.prediction_status) << ",";
        if (job.prediction_outcome.has_value()) {
            out << battle_prediction_outcome_name(*job.prediction_outcome);
        }
        out << "," << (job.prediction_json_valid ? "true" : "false") << ",";
        if (job.single_job_exit_code.has_value()) {
            out << *job.single_job_exit_code;
        }
        out << ",";
        if (job.input_metadata.has_value()) {
            const auto& metadata = *job.input_metadata;
            out << metadata.turn_job_id << ",";
            if (metadata.exec_job_id.has_value()) {
                out << *metadata.exec_job_id;
            }
            out << "," << metadata.wave_id
                << "," << metadata.battle_set_id
                << "," << metadata.entry_savestate_id << ",";
            if (metadata.seed_candidate_id.has_value()) {
                out << *metadata.seed_candidate_id;
            }
            out << "," << metadata.starting_rng_seed
                << "," << battle_prediction_seed_source_name(
                    metadata.seed_source);
        } else {
            out << ",,,,,,,";
        }
        out << ",";
        if (job.final_rng_seed.has_value()) {
            out << *job.final_rng_seed;
        }
        out << ",";
        if (job.total_draws_consumed.has_value()) {
            out << *job.total_draws_consumed;
        }
        out << "," << csv_escape(join(job.partial_reasons, "; "))
            << "," << csv_escape(join(job.diagnostics, "; ")) << "\n";
    }
    return out.str();
}

std::string summary_text(
    const BattlePredictionBatchRunResult& result) {
    std::ostringstream out;
    out << "Battle prediction batch\n";
    out << "  run_name: " << result.options.run_name << "\n";
    out << "  run_root: " << result.options.run_root.string() << "\n";
    out << "  db_root: " << result.options.db_root.string() << "\n";
    out << "  profile: " << result.options.profile_name << "\n";
    if (result.options.scenario_name.has_value()) {
        out << "  scenario: " << *result.options.scenario_name << "\n";
    }
    out << "  jobs: " << result.jobs.size() << "\n";
    out << "  strict_preflight: "
        << (result.preflight_succeeded ? "passed" : "failed") << "\n";
    out << "  require_complete: "
        << (result.options.require_complete ? "true" : "false") << "\n";
    out << "  preflight_only: "
        << (result.options.preflight_only ? "true" : "false") << "\n";
    out << "\n";
    for (const auto& job : result.jobs) {
        out << "job " << job.source_exec_job_id
            << " invocation="
            << battle_prediction_batch_invocation_status_name(
                job.invocation_status)
            << " prediction="
            << battle_prediction_batch_prediction_status_name(
                job.prediction_status);
        if (job.prediction_outcome.has_value()) {
            out << " outcome="
                << battle_prediction_outcome_name(*job.prediction_outcome);
        }
        if (job.input_metadata.has_value()) {
            out << " turn_job=" << job.input_metadata->turn_job_id
                << " seed=" << job.input_metadata->starting_rng_seed
                << " seed_source="
                << battle_prediction_seed_source_name(
                    job.input_metadata->seed_source);
        }
        if (!job.partial_reasons.empty()) {
            out << " partial_reasons=\""
                << join(job.partial_reasons, "; ") << "\"";
        }
        out << "\n";
        for (const auto& diagnostic : job.diagnostics) {
            out << "  diagnostic: " << diagnostic << "\n";
        }
    }
    return out.str();
}

void write_job_manifest_json(
    std::ostream& out,
    const BattlePredictionBatchJobSummary& job) {
    out << "    {\n";
    out << "      \"source_exec_job_id\": " << job.source_exec_job_id << ",\n";
    out << "      \"invocation_status\": \""
        << battle_prediction_batch_invocation_status_name(
            job.invocation_status) << "\",\n";
    out << "      \"prediction_status\": \""
        << battle_prediction_batch_prediction_status_name(
            job.prediction_status) << "\",\n";
    out << "      \"prediction_outcome\": ";
    if (job.prediction_outcome.has_value()) {
        out << "\"" << battle_prediction_outcome_name(
            *job.prediction_outcome) << "\"";
    } else {
        out << "null";
    }
    out << ",\n";
    out << "      \"json_valid\": "
        << (job.prediction_json_valid ? "true" : "false") << ",\n";
    out << "      \"single_job_exit_code\": ";
    write_optional_int(out, job.single_job_exit_code);
    out << ",\n";
    out << "      \"prediction_path\": \""
        << json_escape(job.prediction_path.generic_string()) << "\",\n";
    out << "      \"stderr_path\": \""
        << json_escape(job.stderr_path.generic_string()) << "\",\n";
    out << "      \"partial_reasons\": ";
    write_json_string_array(out, job.partial_reasons);
    out << ",\n";
    out << "      \"diagnostics\": ";
    write_json_string_array(out, job.diagnostics);
    out << ",\n";
    out << "      \"input\": ";
    if (!job.input_metadata.has_value()) {
        out << "null,\n";
    } else {
        const auto& metadata = *job.input_metadata;
        out << "{\n";
        out << "        \"requested_exec_job_id\": ";
        write_optional_i64(out, metadata.requested_exec_job_id);
        out << ",\n";
        out << "        \"turn_job_id\": " << metadata.turn_job_id << ",\n";
        out << "        \"resolved_exec_job_id\": ";
        write_optional_i64(out, metadata.exec_job_id);
        out << ",\n";
        out << "        \"wave_id\": " << metadata.wave_id << ",\n";
        out << "        \"battle_set_id\": " << metadata.battle_set_id << ",\n";
        out << "        \"entry_savestate_id\": "
            << metadata.entry_savestate_id << ",\n";
        out << "        \"entry_savestate_sha256\": ";
        write_optional_string(out, metadata.entry_savestate_sha256);
        out << ",\n";
        out << "        \"turn_index\": " << metadata.turn_index << ",\n";
        out << "        \"seed_candidate_id\": ";
        write_optional_i64(out, metadata.seed_candidate_id);
        out << ",\n";
        out << "        \"starting_rng_seed\": "
            << metadata.starting_rng_seed << ",\n";
        out << "        \"seed_source\": \""
            << battle_prediction_seed_source_name(metadata.seed_source)
            << "\",\n";
        out << "        \"fake_attacks\": " << metadata.fake_attacks << ",\n";
        out << "        \"context_probe_id\": ";
        write_optional_i64(out, metadata.context_probe_id);
        out << ",\n";
        out << "        \"context_source\": \""
            << battle_prediction_context_source_name(metadata.context_source)
            << "\",\n";
        out << "        \"warnings\": ";
        write_json_string_array(out, metadata.warnings);
        out << "\n      },\n";
    }
    out << "      \"final_rng_seed\": ";
    write_optional_u32(out, job.final_rng_seed);
    out << ",\n";
    out << "      \"total_draws_consumed\": ";
    write_optional_int(out, job.total_draws_consumed);
    out << "\n";
    out << "    }";
}

std::string manifest_json(
    const BattlePredictionBatchRunResult& result,
    bool pre_manifest_artifacts_valid,
    bool projected_success) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 2,\n";
    out << "  \"run_name\": \"" << json_escape(result.options.run_name) << "\",\n";
    out << "  \"started_at_utc\": \""
        << json_escape(result.started_at_utc) << "\",\n";
    out << "  \"completed_at_utc\": \""
        << json_escape(result.completed_at_utc) << "\",\n";
    out << "  \"git_commit\": \""
        << json_escape(result.options.git_commit) << "\",\n";
    out << "  \"executable_path\": \""
        << json_escape(result.options.executable_path.generic_string())
        << "\",\n";
    out << "  \"executable\": "
        << executable_fingerprint_json(result.options.executable_path)
        << ",\n";
    out << "  \"db_root\": \""
        << json_escape(result.options.db_root.generic_string()) << "\",\n";
    out << "  \"database_files\": {\n";
    out << "    \"analysis_db\": "
        << file_fingerprint_json(result.options.db_root / "analysis.db") << ",\n";
    out << "    \"state_db\": "
        << file_fingerprint_json(result.options.db_root / "state.db") << ",\n";
    out << "    \"snapshot_manifest\": "
        << file_fingerprint_json(result.options.db_root / "db_manifest.json")
        << "\n";
    out << "  },\n";
    out << "  \"database_identity\": {\n";
    out << "    \"fingerprint_algorithm\": "
           "\"sha256-of-analysis-and-state-file-digests-v1\",\n";
    out << "    \"used_database_fingerprint\": \""
        << json_escape(
            result.database_identity.used_database_fingerprint)
        << "\",\n";
    out << "    \"analysis_db\": {\"size_bytes\":"
        << result.database_identity.analysis_db_size
        << ",\"sha256\":\""
        << json_escape(result.database_identity.analysis_db_sha256)
        << "\"},\n";
    out << "    \"state_db\": {\"size_bytes\":"
        << result.database_identity.state_db_size
        << ",\"sha256\":\""
        << json_escape(result.database_identity.state_db_sha256)
        << "\"},\n";
    out << "    \"snapshot_manifest_present\": "
        << (result.database_identity.snapshot_manifest_present
            ? "true"
            : "false")
        << ",\n";
    out << "    \"snapshot_manifest_verified\": "
        << (result.database_identity.snapshot_manifest_verified
            ? "true"
            : "false")
        << ",\n";
    out << "    \"snapshot_manifest_sha256\": \""
        << json_escape(
            result.database_identity.snapshot_manifest_sha256)
        << "\",\n";
    out << "    \"snapshot_database_fingerprint\": \""
        << json_escape(
            result.database_identity.snapshot_database_fingerprint)
        << "\"\n";
    out << "  },\n";
    out << "  \"profile\": \""
        << json_escape(result.options.profile_name) << "\",\n";
    out << "  \"scenario\": ";
    write_optional_string(out, result.options.scenario_name);
    out << ",\n";
    out << "  \"resource_inputs\": ";
    write_resource_inputs_json(out, result.options.resource_inputs);
    out << ",\n";
    if (uses_legacy_std_json(result.options.resource_inputs)) {
        out << "  \"action_view_std_json_dir\": \""
            << json_escape(
                result.options.action_view_std_json_dir.generic_string())
            << "\",\n";
    }
    out << "  \"allow_seed_candidate_fallback\": "
        << (result.options.allow_seed_candidate_fallback ? "true" : "false")
        << ",\n";
    out << "  \"require_complete\": "
        << (result.options.require_complete ? "true" : "false") << ",\n";
    out << "  \"preflight_only\": "
        << (result.options.preflight_only ? "true" : "false") << ",\n";
    out << "  \"preflight_succeeded\": "
        << (result.preflight_succeeded ? "true" : "false") << ",\n";
    out << "  \"artifacts_valid\": "
        << (pre_manifest_artifacts_valid ? "true" : "false") << ",\n";
    out << "  \"success\": " << (projected_success ? "true" : "false")
        << ",\n";
    out << "  \"errors\": ";
    write_json_string_array(out, result.errors);
    out << ",\n";
    out << "  \"jobs\": [\n";
    for (std::size_t i = 0; i < result.jobs.size(); ++i) {
        if (i != 0) {
            out << ",\n";
        }
        write_job_manifest_json(out, result.jobs[i]);
    }
    out << "\n  ]\n";
    out << "}\n";
    return out.str();
}

} // namespace

const char* battle_prediction_batch_invocation_status_name(
    BattlePredictionBatchInvocationStatus status) {
    switch (status) {
    case BattlePredictionBatchInvocationStatus::NotRun: return "not_run";
    case BattlePredictionBatchInvocationStatus::Ok: return "ok";
    case BattlePredictionBatchInvocationStatus::Error: return "error";
    }
    return "error";
}

const char* battle_prediction_batch_prediction_status_name(
    BattlePredictionBatchPredictionStatus status) {
    switch (status) {
    case BattlePredictionBatchPredictionStatus::NotRun: return "not_run";
    case BattlePredictionBatchPredictionStatus::Complete: return "complete";
    case BattlePredictionBatchPredictionStatus::Partial: return "partial";
    }
    return "partial";
}

std::vector<std::string> validate_battle_prediction_batch_run_options(
    const BattlePredictionBatchRunOptions& supplied) {
    auto resolved = resolve_options(supplied);
    auto errors = std::move(resolved.errors);
    const auto& options = resolved.options;
    if (options.source_exec_job_ids.empty()) {
        errors.push_back("At least one source exec-job ID is required.");
    }
    std::set<long long> seen;
    for (const auto source_exec_job_id : options.source_exec_job_ids) {
        if (source_exec_job_id <= 0) {
            errors.push_back("Source exec-job IDs must be positive.");
        } else if (!seen.insert(source_exec_job_id).second) {
            errors.push_back(
                "Duplicate source exec-job ID: "
                + std::to_string(source_exec_job_id));
        }
    }
    if (options.db_root.empty()) {
        errors.push_back("A DB root is required.");
    } else if (is_mutable_debug_db_root(options.db_root)) {
        errors.push_back(
            "Refusing to use D:/SoaSimDBDebug for prediction; "
            "prepare a snapshot first.");
    } else if (!std::filesystem::is_directory(options.db_root)) {
        errors.push_back(
            "DB root is not an existing directory: "
            + options.db_root.string());
    }
    if (options.profile_name.empty()) {
        errors.push_back("A prediction profile is required.");
    } else if (!battle_prediction_profile_by_name(
            options.profile_name).has_value()) {
        errors.push_back(
            "Unsupported prediction profile: " + options.profile_name);
    }
    if (options.resource_inputs == nullptr) {
        errors.push_back("A predictor resource-input bundle is required.");
    } else if (options.resource_inputs->status
        != BattlePredictorResourceInputStatus::Ready) {
        errors.push_back(
            "Predictor resource-input bundle is not ready: "
            + std::string(battle_predictor_resource_input_status_name(
                options.resource_inputs->status)));
    }
    if (uses_legacy_std_json(options.resource_inputs)) {
        if (options.action_view_std_json_dir.empty()) {
            errors.push_back(
                "Explicit legacy resource inputs require an ActionView "
                "STD JSON directory.");
        } else if (!std::filesystem::is_directory(
                options.action_view_std_json_dir)) {
            errors.push_back(
                "ActionView STD JSON directory is not available: "
                + options.action_view_std_json_dir.string());
        }
    }
    if (options.run_root.empty()) {
        errors.push_back("A batch run root is required.");
    }
    if (options.run_name.empty()) {
        errors.push_back("A batch run name is required.");
    }
    return errors;
}

BattlePredictionBatchRunResult run_battle_prediction_batch(
    const BattlePredictionBatchRunOptions& supplied,
    std::ostream& progress,
    std::ostream& err) {
    auto resolved = resolve_options(supplied);
    BattlePredictionBatchRunResult result;
    result.options = std::move(resolved.options);
    result.started_at_utc = utc_now();
    result.request_path = result.options.run_root / "request.json";
    result.manifest_path = result.options.run_root / "manifest.json";
    result.summary_csv_path = result.options.run_root / "summary.csv";
    result.summary_text_path = result.options.run_root / "summary.txt";

    result.errors = validate_battle_prediction_batch_run_options(
        result.options);
    for (const auto& resolution_error : resolved.errors) {
        if (std::find(
                result.errors.begin(),
                result.errors.end(),
                resolution_error) == result.errors.end()) {
            result.errors.push_back(resolution_error);
        }
    }
    if (!result.errors.empty()) {
        for (const auto& error : result.errors) {
            err << error << "\n";
        }
        result.completed_at_utc = utc_now();
        return result;
    }

    if (!create_run_directories(result.options.run_root, err)) {
        result.errors.push_back("Failed creating the batch run artifact tree.");
        result.completed_at_utc = utc_now();
        return result;
    }

    bool artifacts_ok = write_file(
        result.request_path,
        request_json(result.options, result.started_at_utc),
        err);
    if (!artifacts_ok) {
        result.errors.push_back("Failed writing request.json.");
    }

    progress << "Fingerprinting prediction database inputs.\n";
    const bool database_identity_ok = load_database_identity(
        result.options.db_root,
        &result.database_identity,
        err);
    if (!database_identity_ok) {
        result.errors.push_back(
            "Prediction database identity preflight failed.");
    } else if (!result.database_identity.snapshot_manifest_present) {
        progress
            << "Prediction DB has no db_manifest.json; exact analysis.db "
               "and state.db hashes were recorded directly.\n";
    }

    std::vector<PreflightJob> preflight;
    preflight.reserve(result.options.source_exec_job_ids.size());
    bool all_preflight_ok = database_identity_ok;
    for (const auto source_exec_job_id :
         result.options.source_exec_job_ids) {
        PreflightJob job;
        job.summary.source_exec_job_id = source_exec_job_id;
        job.summary.prediction_path =
            std::filesystem::path("predictions")
            / job_filename(source_exec_job_id, ".json");
        job.summary.stderr_path =
            std::filesystem::path("stderr")
            / job_filename(source_exec_job_id, ".txt");

        if (!database_identity_ok) {
            job.summary.diagnostics.push_back(
                "Not preflighted because prediction database identity "
                "validation failed.");
            preflight.push_back(std::move(job));
            continue;
        }

        BattlePredictionDbInputOptions input_options;
        input_options.db_root = result.options.db_root;
        input_options.selector.exec_job_id = source_exec_job_id;
        input_options.profile_name = result.options.profile_name;
        input_options.scenario_name = result.options.scenario_name;
        input_options.source_selection = result.options.source_selection;
        input_options.expected_encounter = result.options.expected_encounter;
        input_options.resource_inputs = result.options.resource_inputs;
        input_options.allow_seed_candidate_fallback =
            result.options.allow_seed_candidate_fallback;
        input_options.allow_profile_overrides =
            result.options.allow_profile_overrides;
        input_options.emit_causal_diagnostics =
            result.options.emit_causal_diagnostics;

        std::ostringstream job_err;
        job.input = build_battle_prediction_input_from_db_root(
            input_options, job_err);
        if (!job.input.has_value()) {
            all_preflight_ok = false;
            const auto diagnostic = job_err.str().empty()
                ? "Preflight failed without a diagnostic."
                : job_err.str();
            job.summary.diagnostics.push_back(diagnostic);
        } else {
            job.summary.input_metadata = job.input->metadata;
        }
        preflight.push_back(std::move(job));
    }
    result.preflight_succeeded = all_preflight_ok;

    if (!all_preflight_ok) {
        for (auto& job : preflight) {
            if (job.input.has_value()) {
                job.summary.diagnostics.push_back(
                    "Not invoked because strict batch preflight failed "
                    "for another requested job.");
            }
            const auto stderr_contents = join(job.summary.diagnostics, "\n");
            artifacts_ok = write_file(
                result.options.run_root / job.summary.stderr_path,
                stderr_contents.empty()
                    ? std::string{}
                    : stderr_contents + "\n",
                err) && artifacts_ok;
            result.jobs.push_back(std::move(job.summary));
        }
        result.errors.push_back(
            "Strict preflight failed; no predictions were invoked.");
    } else if (result.options.preflight_only) {
        progress << "Batch preflight passed for " << preflight.size()
                 << " source jobs; prediction invocation was skipped.\n";
        for (auto& job : preflight) {
            job.summary.diagnostics.push_back(
                "Prediction not invoked because --preflight-only was requested.");
            const auto stderr_contents = join(job.summary.diagnostics, "\n");
            artifacts_ok = write_file(
                result.options.run_root / job.summary.stderr_path,
                stderr_contents + "\n",
                err) && artifacts_ok;
            result.jobs.push_back(std::move(job.summary));
        }
    } else {
        progress << "Batch preflight passed for " << preflight.size()
                 << " source jobs.\n";
        for (auto& job : preflight) {
            std::ostringstream job_err;
            try {
                const auto prediction = predict_battle(job.input->input);
                job.summary.prediction_outcome = prediction.outcome;
                job.summary.final_rng_seed = prediction.final_rng_seed;
                job.summary.total_draws_consumed =
                    prediction.total_draws_consumed;
                job.summary.single_job_exit_code =
                    single_job_exit_code(prediction);
                job.summary.partial_reasons =
                    prediction_partial_reasons(prediction);
                job.summary.prediction_status =
                    job.summary.partial_reasons.empty()
                    ? BattlePredictionBatchPredictionStatus::Complete
                    : BattlePredictionBatchPredictionStatus::Partial;

                std::ostringstream prediction_json;
                write_battle_prediction_run_json(
                    job.input->metadata, prediction, prediction_json);
                const auto json = prediction_json.str();
                job.summary.prediction_json_valid =
                    looks_like_json_object(json);
                if (!job.summary.prediction_json_valid) {
                    job_err << "Prediction JSON writer did not emit a JSON "
                               "object.\n";
                } else if (!write_file(
                        result.options.run_root / job.summary.prediction_path,
                        json,
                        job_err)) {
                    job.summary.prediction_json_valid = false;
                }

                if (job.summary.prediction_json_valid) {
                    job.summary.invocation_status =
                        BattlePredictionBatchInvocationStatus::Ok;
                } else {
                    job.summary.invocation_status =
                        BattlePredictionBatchInvocationStatus::Error;
                    artifacts_ok = false;
                }
            } catch (const std::exception& exception) {
                job.summary.invocation_status =
                    BattlePredictionBatchInvocationStatus::Error;
                artifacts_ok = false;
                job_err << "Prediction threw an exception: "
                        << exception.what() << "\n";
            } catch (...) {
                job.summary.invocation_status =
                    BattlePredictionBatchInvocationStatus::Error;
                artifacts_ok = false;
                job_err << "Prediction threw an unknown exception.\n";
            }

            if (!job_err.str().empty()) {
                job.summary.diagnostics.push_back(job_err.str());
            }
            const auto stderr_contents = join(job.summary.diagnostics, "\n");
            artifacts_ok = write_file(
                result.options.run_root / job.summary.stderr_path,
                stderr_contents.empty()
                    ? std::string{}
                    : stderr_contents + "\n",
                err) && artifacts_ok;
            progress << "Predicted source exec job "
                     << job.summary.source_exec_job_id << ": invocation="
                     << battle_prediction_batch_invocation_status_name(
                         job.summary.invocation_status)
                     << " prediction="
                     << battle_prediction_batch_prediction_status_name(
                         job.summary.prediction_status)
                     << "\n";
            result.jobs.push_back(std::move(job.summary));
        }
    }

    const bool all_invocations_ok = result.options.preflight_only
        || std::all_of(
            result.jobs.begin(),
            result.jobs.end(),
            [](const BattlePredictionBatchJobSummary& job) {
                return job.invocation_status
                    == BattlePredictionBatchInvocationStatus::Ok;
            });
    const bool all_predictions_complete = result.options.preflight_only
        || std::all_of(
            result.jobs.begin(),
            result.jobs.end(),
            [](const BattlePredictionBatchJobSummary& job) {
                return job.prediction_status
                    == BattlePredictionBatchPredictionStatus::Complete;
            });

    result.completed_at_utc = utc_now();
    artifacts_ok = write_file(
        result.summary_csv_path, summary_csv(result.jobs), err) && artifacts_ok;
    artifacts_ok = write_file(
        result.summary_text_path, summary_text(result), err) && artifacts_ok;

    const bool projected_success =
        result.preflight_succeeded
        && all_invocations_ok
        && artifacts_ok
        && (!result.options.require_complete || all_predictions_complete);
    const auto manifest = manifest_json(
        result, artifacts_ok, projected_success);
    artifacts_ok = write_file(
        result.manifest_path, manifest, err) && artifacts_ok;

    result.artifacts_valid = artifacts_ok;
    result.success =
        result.preflight_succeeded
        && all_invocations_ok
        && result.artifacts_valid
        && (!result.options.require_complete || all_predictions_complete);
    result.recommended_exit_code = result.success ? 0 : 1;
    return result;
}

} // namespace savor::predict
