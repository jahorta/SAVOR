#include "BattlePredictorCli.h"

#include "BattleJobRunOptions.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include <Analysis/SqliteAnalysisDb.h>

#include <sqlite3.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace savor::predict {
namespace {

struct DbJobPredictionSource {
    long long turn_job_id = 0;
    std::optional<long long> exec_job_id;
    long long wave_id = 0;
    int fake_attacks = 0;
    std::uint32_t start_seed = 0;
    std::string turn_commands_hex;
};

class SqliteHandle {
public:
    SqliteHandle() = default;
    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;
    ~SqliteHandle() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    bool open_readonly(const std::filesystem::path& path, std::ostream& err) {
        const int rc = sqlite3_open_v2(
            path.string().c_str(),
            &db_,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (rc != SQLITE_OK) {
            err << "Failed opening analysis DB " << path.string() << ": "
                << (db_ == nullptr ? "sqlite error" : sqlite3_errmsg(db_)) << "\n";
            return false;
        }
        sqlite3_busy_timeout(db_, 5000);
        return true;
    }

    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool prepare(sqlite3* db, const char* sql, Statement* out, std::ostream& err) {
    if (sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr) != SQLITE_OK) {
        err << "Failed preparing SQL: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return true;
}

bool parse_ll(std::string_view value, long long& out) {
    std::string owned(value);
    char* end = nullptr;
    out = std::strtoll(owned.c_str(), &end, 10);
    return end != owned.c_str() && *end == '\0';
}

bool parse_int(std::string_view value, int& out) {
    std::string owned(value);
    char* end = nullptr;
    const long parsed = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_u32_seed(std::string_view value, std::uint32_t& out) {
    std::string normalized(value);
    int base = 10;
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized.erase(0, 2);
        base = 16;
    }
    if (normalized.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(normalized.c_str(), &end, base);
    if (end == normalized.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint32_t>(parsed);
    return parsed <= 0xFFFFFFFFul;
}

bool require_value(
    const std::vector<std::string>& args,
    std::size_t& index,
    const std::string& option,
    std::string& value,
    std::vector<std::string>& errors) {
    if (index + 1 >= args.size()) {
        errors.push_back(option + " requires a value.");
        return false;
    }
    value = args[++index];
    return true;
}

bool read_binary_file(const std::filesystem::path& path, std::string& out, std::ostream& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err << "Failed opening " << path.string() << "\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

std::optional<std::string> column_text_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = sqlite3_column_text(st, column);
    return text == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(text));
}

std::optional<long long> column_i64_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, column);
}

std::optional<DbJobPredictionSource> read_db_job_source(
    sqlite3* db,
    const BattlePredictorCliOptions& options,
    std::ostream& err) {
    constexpr const char* kSql = R"SQL(
        SELECT
            j.turn_job_id,
            j.exec_job_id,
            j.wave_id,
            j.fake_attacks_this_turn,
            COALESCE(u.seed_value, sc.seed_value),
            COALESCE(j.resolved_turn_commands_blob, '')
        FROM ab_turn_job j
        JOIN ab_turn_wave w ON w.wave_id = j.wave_id
        JOIN ab_seed_candidate sc ON sc.seed_candidate_id = COALESCE(j.seed_candidate_id, w.seed_candidate_id)
        LEFT JOIN sp_probe_run pr ON pr.entry_savestate_id = (
            SELECT entry_savestate_id FROM ab_battle_set WHERE battle_set_id = w.battle_set_id
        )
        LEFT JOIN sp_probe_result res ON res.probe_run_id = pr.probe_run_id
        LEFT JOIN sp_unique_seed u ON u.probe_result_id = res.probe_result_id
            AND u.input_frame_id = sc.source_input_frame_id
        WHERE (?1 IS NOT NULL AND j.turn_job_id = ?1)
           OR (?2 IS NOT NULL AND j.exec_job_id = ?2)
        LIMIT 1;
    )SQL";

    Statement st;
    if (!prepare(db, kSql, &st, err)) {
        return std::nullopt;
    }
    if (options.turn_job_id.has_value()) {
        sqlite3_bind_int64(st.st, 1, *options.turn_job_id);
    } else {
        sqlite3_bind_null(st.st, 1);
    }
    if (options.exec_job_id.has_value()) {
        sqlite3_bind_int64(st.st, 2, *options.exec_job_id);
    } else {
        sqlite3_bind_null(st.st, 2);
    }

    const int rc = sqlite3_step(st.st);
    if (rc == SQLITE_DONE) {
        err << "No matching ab_turn_job found.\n";
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        err << "Failed reading ab_turn_job: " << sqlite3_errmsg(db) << "\n";
        return std::nullopt;
    }

    DbJobPredictionSource source;
    source.turn_job_id = sqlite3_column_int64(st.st, 0);
    source.exec_job_id = column_i64_optional(st.st, 1);
    source.wave_id = sqlite3_column_int64(st.st, 2);
    source.fake_attacks = sqlite3_column_int(st.st, 3);
    source.start_seed = static_cast<std::uint32_t>(sqlite3_column_int64(st.st, 4));
    source.turn_commands_hex = column_text_optional(st.st, 5).value_or("");
    return source;
}

bool load_context_from_db(
    savor::db::analysis::SqliteAnalysisDb& analysis_db,
    const DbJobPredictionSource& source,
    soa::battle::ctx::BattleContext& context,
    std::ostream& err) {
    std::optional<savor::db::BattleContextProbeSnapshot> probe;
    const auto wave = analysis_db.GetBattleTurnWave(source.wave_id);
    if (wave.has_value() && wave->context_probe_id.has_value()) {
        probe = analysis_db.GetBattleContextProbe(*wave->context_probe_id);
    }
    if (!probe.has_value()) {
        probe = analysis_db.GetLatestBattleContextForWave(source.wave_id);
    }
    if (!probe.has_value() || !probe->context_blob.has_value() || probe->context_blob->empty()) {
        err << "No completed battle context blob is available for wave " << source.wave_id << ".\n";
        return false;
    }
    if (!soa::battle::ctx::codec::decode(*probe->context_blob, context)) {
        err << "Failed decoding battle context blob for wave " << source.wave_id << ".\n";
        return false;
    }
    return true;
}

bool build_input_from_context_file(
    const BattlePredictorCliOptions& options,
    BattlePredictionInput& input,
    std::ostream& err) {
    if (!options.start_seed.has_value()) {
        err << "--start-seed is required with --context-file.\n";
        return false;
    }
    if (!options.fake_attacks.has_value()) {
        err << "--fake-attacks is required with --context-file.\n";
        return false;
    }

    std::string context_blob;
    if (!read_binary_file(options.context_file, context_blob, err)) {
        return false;
    }
    if (!soa::battle::ctx::codec::decode(context_blob, input.context)) {
        err << "Failed decoding context file " << options.context_file.string() << ".\n";
        return false;
    }

    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(options.turn_plan_hex);
    if (!commands.has_value()) {
        err << "Failed decoding --turn-plan-hex as BattleTurnCommandSet.\n";
        return false;
    }

    input.starting_rng_seed = *options.start_seed;
    input.turn_plan.fake_attack_count = static_cast<std::uint32_t>(*options.fake_attacks);
    input.turn_plan.commands = *commands;
    return true;
}

bool build_input_from_db_job(
    const BattlePredictorCliOptions& options,
    BattlePredictionInput& input,
    std::ostream& err) {
    const auto analysis_db_path = options.db_root / "analysis.db";
    SqliteHandle db;
    if (!db.open_readonly(analysis_db_path, err)) {
        return false;
    }
    savor::db::analysis::SqliteAnalysisDb analysis_db(db.get());

    const auto source = read_db_job_source(db.get(), options, err);
    if (!source.has_value()) {
        return false;
    }
    if (source->turn_commands_hex.empty()) {
        err << "Selected turn job has no resolved turn command blob.\n";
        return false;
    }
    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(source->turn_commands_hex);
    if (!commands.has_value()) {
        err << "Selected turn job command blob could not be decoded.\n";
        return false;
    }
    if (!load_context_from_db(analysis_db, *source, input.context, err)) {
        return false;
    }

    input.starting_rng_seed = options.start_seed.value_or(source->start_seed);
    input.turn_plan.fake_attack_count = static_cast<std::uint32_t>(
        options.fake_attacks.value_or(source->fake_attacks));
    input.turn_plan.commands = *commands;
    return true;
}

} // namespace

BattlePredictorCliParseResult parse_predict_battle_tokens(const std::vector<std::string>& args) {
    BattlePredictorCliParseResult result;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        std::string value;
        if (arg == "--context-file") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.context_file = value;
            }
        } else if (arg == "--turn-plan-hex") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.turn_plan_hex = value;
            }
        } else if (arg == "--fake-attacks") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed) && parsed >= 0) {
                result.options.fake_attacks = parsed;
            } else {
                result.errors.push_back("--fake-attacks requires a non-negative integer.");
            }
        } else if (arg == "--start-seed") {
            std::uint32_t parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_u32_seed(value, parsed)) {
                result.options.start_seed = parsed;
            } else {
                result.errors.push_back("--start-seed requires a decimal or hex uint32.");
            }
        } else if (arg == "--turn-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.turn_job_id = parsed;
            } else {
                result.errors.push_back("--turn-job-id requires an integer.");
            }
        } else if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.exec_job_id = parsed;
            } else {
                result.errors.push_back("--exec-job-id requires an integer.");
            }
        } else if (arg == "--db-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.db_root = value;
            }
        } else if (arg == "--profile") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.profile_name = value;
            }
        } else if (arg == "--format") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (value == "text") {
                    result.options.json = false;
                } else if (value == "json") {
                    result.options.json = true;
                } else {
                    result.errors.push_back("--format must be text or json.");
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back("Unknown predict-battle option: " + arg);
        }
    }

    if (!result.help_requested) {
        const auto validation = validate_predict_battle_options(result.options);
        result.errors.insert(result.errors.end(), validation.begin(), validation.end());
    }
    return result;
}

std::vector<std::string> validate_predict_battle_options(const BattlePredictorCliOptions& options) {
    std::vector<std::string> errors;
    const bool has_context_file = !options.context_file.empty();
    const bool has_turn = options.turn_job_id.has_value();
    const bool has_exec = options.exec_job_id.has_value();
    const bool has_db_selector = has_turn || has_exec;

    if (has_turn && has_exec) {
        errors.push_back("Specify only one of --turn-job-id or --exec-job-id.");
    }
    if (has_context_file == has_db_selector) {
        errors.push_back("Specify either --context-file with --turn-plan-hex, or one DB job selector.");
    }
    if (has_context_file && options.turn_plan_hex.empty()) {
        errors.push_back("--turn-plan-hex is required with --context-file.");
    }
    if (has_db_selector && is_mutable_debug_db_root(options.db_root)) {
        errors.push_back("Refusing to use D:/SoaSimDBDebug for prediction; use D:/SavorPredictDB.");
    }
    if (has_turn && *options.turn_job_id <= 0) {
        errors.push_back("--turn-job-id must be positive.");
    }
    if (has_exec && *options.exec_job_id <= 0) {
        errors.push_back("--exec-job-id must be positive.");
    }
    if (!battle_prediction_profile_by_name(options.profile_name).has_value()) {
        errors.push_back("Unsupported --profile: " + options.profile_name);
    }
    return errors;
}

int run_predict_battle(const BattlePredictorCliOptions& options, std::ostream& out, std::ostream& err) {
    const auto profile = battle_prediction_profile_by_name(options.profile_name);
    if (!profile.has_value()) {
        err << "Unsupported profile: " << options.profile_name << "\n";
        return 2;
    }

    BattlePredictionInput input;
    input.profile = *profile;

    const bool from_context_file = !options.context_file.empty();
    const bool ok = from_context_file
        ? build_input_from_context_file(options, input, err)
        : build_input_from_db_job(options, input, err);
    if (!ok) {
        return 1;
    }

    const auto result = predict_battle(input);
    if (options.json) {
        write_battle_prediction_json(result, out);
    } else {
        write_battle_prediction_text(result, out);
    }
    return result.has_unsupported_events || !result.errors.empty() ? 1 : 0;
}

} // namespace savor::predict
