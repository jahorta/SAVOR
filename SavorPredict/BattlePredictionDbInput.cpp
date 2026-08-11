#include "BattlePredictionDbInput.h"

#include "DbCopy.h"
#include "BattlePredictionScenario.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include <Analysis/SqliteAnalysisDb.h>

#include <sqlite3.h>

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>

namespace savor::predict {
namespace {

class SqliteReadHandle {
public:
    SqliteReadHandle() = default;
    SqliteReadHandle(const SqliteReadHandle&) = delete;
    SqliteReadHandle& operator=(const SqliteReadHandle&) = delete;

    ~SqliteReadHandle() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    bool open(const std::filesystem::path& path, std::ostream& err) {
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
        sqlite3_extended_result_codes(db_, 1);
        return true;
    }

    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

std::string hex_seed(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

void write_json_string_field(
    std::ostream& out,
    const char* name,
    std::string_view value,
    bool& first) {
    if (!first) {
        out << ",\n";
    }
    first = false;
    out << "    \"" << name << "\": \"" << json_escape(value) << "\"";
}

void write_json_i64_field(
    std::ostream& out,
    const char* name,
    long long value,
    bool& first) {
    if (!first) {
        out << ",\n";
    }
    first = false;
    out << "    \"" << name << "\": " << value;
}

void write_json_int_field(
    std::ostream& out,
    const char* name,
    int value,
    bool& first) {
    if (!first) {
        out << ",\n";
    }
    first = false;
    out << "    \"" << name << "\": " << value;
}

void write_json_u32_field(
    std::ostream& out,
    const char* name,
    std::uint32_t value,
    bool& first) {
    if (!first) {
        out << ",\n";
    }
    first = false;
    out << "    \"" << name << "\": " << value;
}

std::uint32_t checked_seed_from_i64(std::int64_t value) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0xFFFFFFFFull);
}

bool report_sqlite_query_error(
    sqlite3* sqlite_db,
    std::string_view operation,
    std::ostream& err) {
    if (sqlite_db == nullptr) {
        return false;
    }
    const int rc = sqlite3_extended_errcode(sqlite_db);
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return false;
    }
    err << operation << " failed: " << sqlite3_errmsg(sqlite_db)
        << " (sqlite rc=" << rc << ").\n";
    return true;
}

std::optional<savor::db::BattleTurnJobSnapshot> resolve_turn_job(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionJobSelector& selector,
    sqlite3* sqlite_db,
    std::ostream& err) {
    if (selector.turn_job_id.has_value() == selector.exec_job_id.has_value()) {
        err << "Specify exactly one DB job selector.\n";
        return std::nullopt;
    }
    if (selector.turn_job_id.has_value()) {
        auto row = analysis_db.GetBattleTurnJob(*selector.turn_job_id);
        if (!row.has_value()) {
            if (!report_sqlite_query_error(
                    sqlite_db,
                    "Querying ab_turn_job by turn_job_id "
                        + std::to_string(*selector.turn_job_id),
                    err)) {
                err << "No matching ab_turn_job found for turn_job_id "
                    << *selector.turn_job_id << ".\n";
            }
        }
        return row;
    }

    auto row = analysis_db.GetBattleTurnJobForExecJob(*selector.exec_job_id);
    if (!row.has_value()) {
        if (!report_sqlite_query_error(
                sqlite_db,
                "Querying ab_turn_job by exec_job_id "
                    + std::to_string(*selector.exec_job_id),
                err)) {
            err << "No matching ab_turn_job found for exec_job_id "
                << *selector.exec_job_id << ".\n";
        }
    }
    return row;
}

std::optional<savor::db::BattleContextProbeSnapshot> resolve_context_probe(
    const savor::db::IAnalysisDb& analysis_db,
    const savor::db::BattleTurnWaveSnapshot& wave,
    BattlePredictionDbInputMetadata& metadata,
    sqlite3* sqlite_db,
    bool* query_failed,
    std::ostream& err) {
    *query_failed = false;
    if (wave.context_probe_id.has_value()) {
        auto probe = analysis_db.GetBattleContextProbe(*wave.context_probe_id);
        if (!probe.has_value()
            && report_sqlite_query_error(
                sqlite_db,
                "Querying ab_battle_context_probe by context_probe_id "
                    + std::to_string(*wave.context_probe_id),
                err)) {
            *query_failed = true;
            return std::nullopt;
        }
        if (probe.has_value()
            && probe->probe_status == savor::db::BattleContextProbeStatus::Succeeded
            && probe->context_blob.has_value()
            && !probe->context_blob->empty()) {
            metadata.context_source = BattlePredictionContextSource::WaveContextProbe;
            return probe;
        }
        metadata.warnings.push_back(
            "wave context_probe_id did not resolve to a completed context blob; falling back to latest succeeded context for wave");
    }

    auto probe = analysis_db.GetLatestBattleContextForWave(wave.wave_id);
    if (!probe.has_value()
        && report_sqlite_query_error(
            sqlite_db,
            "Querying latest succeeded ab_battle_context_probe for wave "
                + std::to_string(wave.wave_id),
            err)) {
        *query_failed = true;
        return std::nullopt;
    }
    if (probe.has_value()) {
        metadata.context_source = BattlePredictionContextSource::LatestWaveContextProbe;
    }
    return probe;
}

std::optional<std::string> lookup_savestate_sha256(
    sqlite3* state_db,
    std::int64_t savestate_id,
    std::ostream& err) {
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT a.sha256 "
        "FROM state_savestate s "
        "JOIN state_artifact a ON a.artifact_id=s.artifact_id "
        "WHERE s.savestate_id=?1 AND s.is_complete=1;";
    if (sqlite3_prepare_v2(state_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        err << "Failed preparing entry-savestate fingerprint query: "
            << sqlite3_errmsg(state_db) << "\n";
        return std::nullopt;
    }
    sqlite3_bind_int64(statement, 1, savestate_id);
    std::optional<std::string> result;
    const int step_rc = sqlite3_step(statement);
    if (step_rc == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(statement, 0);
        if (value != nullptr) {
            result = reinterpret_cast<const char*>(value);
        }
    } else if (step_rc != SQLITE_DONE) {
        err << "Failed querying entry-savestate fingerprint: "
            << sqlite3_errmsg(state_db)
            << " (sqlite rc=" << sqlite3_extended_errcode(state_db)
            << ").\n";
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    sqlite3_finalize(statement);
    if (!result.has_value() || result->empty()) {
        err << "No complete state_savestate artifact fingerprint found for savestate "
            << savestate_id << ".\n";
        return std::nullopt;
    }
    return result;
}

} // namespace

const char* battle_prediction_seed_source_name(BattlePredictionSeedSource source) {
    switch (source) {
    case BattlePredictionSeedSource::Override: return "override";
    case BattlePredictionSeedSource::SeedProbeConfirmedResult: return "sp_probe_result.seed_value";
    case BattlePredictionSeedSource::SeedCandidate: return "ab_seed_candidate.seed_value";
    case BattlePredictionSeedSource::Unknown: return "unknown";
    }
    return "unknown";
}

const char* battle_prediction_fake_attack_source_name(BattlePredictionFakeAttackSource source) {
    switch (source) {
    case BattlePredictionFakeAttackSource::Override: return "override";
    case BattlePredictionFakeAttackSource::TurnJob: return "turn_job";
    case BattlePredictionFakeAttackSource::Unknown: return "unknown";
    }
    return "unknown";
}

const char* battle_prediction_context_source_name(BattlePredictionContextSource source) {
    switch (source) {
    case BattlePredictionContextSource::WaveContextProbe: return "wave_context_probe";
    case BattlePredictionContextSource::LatestWaveContextProbe: return "latest_wave_context_probe";
    case BattlePredictionContextSource::Unknown: return "unknown";
    }
    return "unknown";
}

static std::optional<BattlePredictionDbInput>
build_battle_prediction_input_from_analysis_db_impl(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionDbInputOptions& options,
    sqlite3* sqlite_db,
    std::ostream& err) {
    const auto profile = battle_prediction_profile_by_name(options.profile_name);
    if (!profile.has_value()) {
        err << "Unsupported profile: " << options.profile_name << "\n";
        return std::nullopt;
    }

    std::vector<std::string> profile_override_warnings;
    const auto check_profile_constraint = [&](bool satisfied, const std::string& message) {
        if (satisfied) {
            return true;
        }
        if (!options.allow_profile_overrides) {
            err << message << "\n";
            return false;
        }
        profile_override_warnings.push_back(message);
        return true;
    };

    std::optional<BattlePredictionScenario> resolved_scenario;
    if (options.scenario_name.has_value()) {
        const auto scenario = battle_prediction_scenario_by_name(*options.scenario_name);
        if (!scenario.has_value()) {
            err << "Unsupported scenario: " << *options.scenario_name << "\n";
            return std::nullopt;
        }
        resolved_scenario = *scenario;
        if (!check_profile_constraint(
                scenario->profile_name == profile->name,
                "Scenario " + scenario->name + " requires profile "
                    + scenario->profile_name)) {
            return std::nullopt;
        }
    }

    auto turn_job =
        resolve_turn_job(analysis_db, options.selector, sqlite_db, err);
    if (!turn_job.has_value()) {
        return std::nullopt;
    }

    const auto wave = analysis_db.GetBattleTurnWave(turn_job->wave_id);
    if (!wave.has_value()) {
        if (report_sqlite_query_error(
                sqlite_db,
                "Querying ab_turn_wave by wave_id "
                    + std::to_string(turn_job->wave_id),
                err)) {
            return std::nullopt;
        }
        err << "Selected turn job references missing wave " << turn_job->wave_id << ".\n";
        return std::nullopt;
    }
    if (!check_profile_constraint(
            wave->turn_index == profile->supported_turn_index,
            "Profile " + profile->name + " requires turn_index "
                + std::to_string(profile->supported_turn_index)
                + "; selected job uses turn_index "
                + std::to_string(wave->turn_index))) {
        return std::nullopt;
    }
    const auto battle_set = analysis_db.GetBattleSet(wave->battle_set_id);
    if (!battle_set.has_value()) {
        if (report_sqlite_query_error(
                sqlite_db,
                "Querying ab_battle_set by battle_set_id "
                    + std::to_string(wave->battle_set_id),
                err)) {
            return std::nullopt;
        }
        err << "Selected turn job references missing battle set " << wave->battle_set_id << ".\n";
        return std::nullopt;
    }

    const auto seed_candidate_id = turn_job->seed_candidate_id.value_or(wave->seed_candidate_id);
    const auto seed_candidate = analysis_db.GetBattleSeedCandidate(seed_candidate_id);
    if (!seed_candidate.has_value()) {
        if (report_sqlite_query_error(
                sqlite_db,
                "Querying ab_seed_candidate by seed_candidate_id "
                    + std::to_string(seed_candidate_id),
                err)) {
            return std::nullopt;
        }
        err << "Selected turn job references seed candidate "
            << seed_candidate_id << " could not be loaded.\n";
        return std::nullopt;
    }

    BattlePredictionDbInput resolved;
    resolved.input.profile = *profile;
    resolved.input.scenario_name = resolved_scenario.has_value()
        ? std::optional<std::string>{resolved_scenario->name}
        : std::nullopt;
    resolved.input.source_selection = options.source_selection;
    if (!resolved.input.source_selection.has_value()) {
        const auto source_scenario = resolved_scenario.has_value()
            ? resolved_scenario
            : battle_prediction_scenario_by_name(profile->name);
        if (source_scenario.has_value()) {
            resolved.input.source_selection = source_scenario->source_selection;
        }
    }
    resolved.input.source_validation.expected_encounter = options.expected_encounter;
    resolved.input.turn_index = wave->turn_index;
    resolved.input.resource_inputs = options.resource_inputs;
    resolved.input.options.allow_profile_overrides = options.allow_profile_overrides;
    resolved.input.options.emit_causal_diagnostics =
        options.emit_causal_diagnostics;
    auto& metadata = resolved.metadata;
    metadata.source_db_root = options.db_root;
    metadata.profile_name = profile->name;
    metadata.scenario_name = resolved.input.scenario_name;
    metadata.source_selection = resolved.input.source_selection;
    metadata.expected_encounter = options.expected_encounter;
    metadata.requested_turn_job_id = options.selector.turn_job_id;
    metadata.requested_exec_job_id = options.selector.exec_job_id;
    metadata.turn_job_id = turn_job->turn_job_id;
    metadata.exec_job_id = turn_job->exec_job_id;
    metadata.wave_id = wave->wave_id;
    metadata.battle_set_id = wave->battle_set_id;
    metadata.entry_savestate_id = battle_set->entry_savestate_id;
    metadata.turn_index = wave->turn_index;
    metadata.seed_candidate_id = seed_candidate_id;
    metadata.resolved_turn_variant_key = turn_job->resolved_turn_variant_key;
    metadata.warnings.insert(
        metadata.warnings.end(),
        profile_override_warnings.begin(),
        profile_override_warnings.end());

    resolved.input.start_boundary = options.start_seed_override.has_value()
        ? BattlePredictionStartBoundary::BattleCoordinatorStart
        : BattlePredictionStartBoundary::CapturedTurnStart;
    metadata.start_boundary = resolved.input.start_boundary;

    if (options.start_seed_override.has_value()) {
        resolved.input.starting_rng_seed = *options.start_seed_override;
        metadata.seed_source = BattlePredictionSeedSource::Override;
        if (turn_job->rng_seed.has_value()) {
            metadata.warnings.push_back("start seed override ignored stored turn-job RNG seed");
        }
    } else if (seed_candidate->source_kind
        == savor::db::BattleSeedCandidateSourceKind::SeedProbeConfirmedResult) {
        if (!seed_candidate->source_probe_result_id.has_value()) {
            err << "Selected SeedProbe candidate has no source_probe_result_id.\n";
            return std::nullopt;
        }
        const auto probe_result =
            analysis_db.GetSeedProbeResult(*seed_candidate->source_probe_result_id);
        if (!probe_result.has_value()) {
            if (report_sqlite_query_error(
                    sqlite_db,
                    "Querying sp_probe_result by probe_result_id "
                        + std::to_string(*seed_candidate->source_probe_result_id),
                    err)) {
                return std::nullopt;
            }
            err << "Selected SeedProbe candidate references missing sp_probe_result "
                << *seed_candidate->source_probe_result_id << ".\n";
            return std::nullopt;
        }
        if (probe_result->evidence_state != savor::db::SeedProbeEvidenceState::Confirmed
            || probe_result->confirmation_of_probe_result_id.has_value()) {
            err << "Selected SeedProbe candidate source "
                << probe_result->probe_result_id
                << " is not a confirmed representative observation.\n";
            return std::nullopt;
        }
        if ((seed_candidate->source_input_frame_id.has_value()
                && *seed_candidate->source_input_frame_id
                    != probe_result->input_frame_id)
            || seed_candidate->seed_value
                != static_cast<std::int64_t>(probe_result->seed_value)) {
            err << "Selected SeedProbe candidate facts do not match sp_probe_result "
                << probe_result->probe_result_id << ".\n";
            return std::nullopt;
        }
        const auto probe_run = analysis_db.GetSeedProbeRun(probe_result->probe_run_id);
        if (!probe_run.has_value()
            || probe_run->entry_savestate_id != battle_set->entry_savestate_id
            || (probe_run->status != savor::db::SeedProbeRunStatus::Completed
                && probe_run->status
                    != savor::db::SeedProbeRunStatus::CompletedPartial)) {
            err << "Selected SeedProbe result does not belong to a completed run "
                   "for the battle-set entry savestate.\n";
            return std::nullopt;
        }
        const auto accepted_frames =
            analysis_db.ListAnalysisInputSetFrames(probe_run->accepted_input_set_id);
        const auto accepted = std::find_if(
            accepted_frames.begin(),
            accepted_frames.end(),
            [&](const savor::db::AnalysisInputSetFrameRow& frame) {
                return frame.input_frame_id == probe_result->input_frame_id;
            });
        if (accepted == accepted_frames.end()) {
            err << "Selected SeedProbe result's input frame is not in the run's "
                   "accepted input set.\n";
            return std::nullopt;
        }
        resolved.input.starting_rng_seed = probe_result->seed_value;
        metadata.seed_source =
            BattlePredictionSeedSource::SeedProbeConfirmedResult;
    } else {
        if (seed_candidate->source_probe_result_id.has_value()) {
            err << "Only SeedProbe-confirmed candidates may reference "
                   "source_probe_result_id.\n";
            return std::nullopt;
        }
        resolved.input.starting_rng_seed =
            checked_seed_from_i64(seed_candidate->seed_value);
        metadata.seed_source = BattlePredictionSeedSource::SeedCandidate;
        if (turn_job->rng_seed.has_value()) {
            metadata.warnings.push_back(
                "using ab_seed_candidate.seed_value as battle start seed; stored turn-job RNG seed is not used for predictor input");
        }
    }
    metadata.starting_rng_seed = resolved.input.starting_rng_seed;

    if (options.fake_attacks_override.has_value()) {
        resolved.input.turn_plan.fake_attack_count =
            static_cast<std::uint32_t>(*options.fake_attacks_override);
        metadata.fake_attacks = *options.fake_attacks_override;
        metadata.fake_attack_source = BattlePredictionFakeAttackSource::Override;
    } else {
        resolved.input.turn_plan.fake_attack_count =
            static_cast<std::uint32_t>(turn_job->fake_attacks_this_turn);
        metadata.fake_attacks = turn_job->fake_attacks_this_turn;
        metadata.fake_attack_source = BattlePredictionFakeAttackSource::TurnJob;
    }
    if (!turn_job->resolved_turn_commands_blob.has_value() || turn_job->resolved_turn_commands_blob->empty()) {
        err << "Selected turn job has no resolved turn command blob.\n";
        return std::nullopt;
    }
    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(
        *turn_job->resolved_turn_commands_blob);
    if (!commands.has_value()) {
        err << "Selected turn job command blob could not be decoded.\n";
        return std::nullopt;
    }
    resolved.input.turn_plan.commands = *commands;

    bool context_query_failed = false;
    const auto probe = resolve_context_probe(
        analysis_db,
        *wave,
        metadata,
        sqlite_db,
        &context_query_failed,
        err);
    if (context_query_failed) {
        return std::nullopt;
    }
    if (!probe.has_value() || !probe->context_blob.has_value() || probe->context_blob->empty()) {
        err << "No completed battle context blob is available for wave " << wave->wave_id << ".\n";
        return std::nullopt;
    }
    metadata.context_probe_id = probe->context_probe_id;
    metadata.context_version = probe->context_version;
    metadata.context_status = std::string(savor::db::ToDbString(probe->probe_status));

    if (!soa::battle::ctx::codec::decode(*probe->context_blob, resolved.input.context)) {
        err << "Failed decoding battle context blob for wave " << wave->wave_id << ".\n";
        return std::nullopt;
    }

    return resolved;
}

std::optional<BattlePredictionDbInput> build_battle_prediction_input_from_analysis_db(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionDbInputOptions& options,
    std::ostream& err) {
    return build_battle_prediction_input_from_analysis_db_impl(
        analysis_db, options, nullptr, err);
}

std::optional<BattlePredictionDbInput> build_battle_prediction_input_from_db_root(
    const BattlePredictionDbInputOptions& options,
    std::ostream& err) {
    if (is_mutable_debug_db_root(options.db_root)) {
        err << "Refusing to use D:/SoaSimDBDebug for prediction; use D:/SavorPredictDB.\n";
        return std::nullopt;
    }

    const auto analysis_db_path = options.db_root / "analysis.db";
    SqliteReadHandle handle;
    if (!handle.open(analysis_db_path, err)) {
        return std::nullopt;
    }
    savor::db::analysis::SqliteAnalysisDb analysis_db(handle.get());
    auto resolved = build_battle_prediction_input_from_analysis_db_impl(
        analysis_db, options, handle.get(), err);
    if (!resolved.has_value()) {
        return std::nullopt;
    }

    SqliteReadHandle state_handle;
    if (!state_handle.open(options.db_root / "state.db", err)) {
        return std::nullopt;
    }
    const auto savestate_sha256 = lookup_savestate_sha256(
        state_handle.get(), resolved->metadata.entry_savestate_id, err);
    if (!savestate_sha256.has_value()) {
        return std::nullopt;
    }
    resolved->metadata.entry_savestate_sha256 = *savestate_sha256;
    resolved->input.source_validation.savestate_sha256 = *savestate_sha256;
    return resolved;
}

void write_battle_prediction_db_metadata_text(
    const BattlePredictionDbInputMetadata& metadata,
    std::ostream& out) {
    out << "DB-backed prediction input\n";
    out << "  db_root: " << metadata.source_db_root.string() << "\n";
    out << "  profile: " << metadata.profile_name << "\n";
    if (metadata.scenario_name.has_value()) {
        out << "  scenario: " << *metadata.scenario_name << "\n";
    }
    if (metadata.source_selection.has_value()) {
        out << "  source_producer: "
            << battle_source_producer_kind_name(
                metadata.source_selection->producer_kind)
            << "\n";
        if (metadata.source_selection->producer_kind
            == BattleSourceProducerKind::ScriptedBattleRequest) {
            out << "  source_script: "
                << metadata.source_selection->scripted_request.script_identity << "\n";
            out << "  source_section: "
                << metadata.source_selection->scripted_request.section_identity << "\n";
            out << "  source_payload_offset: "
                << metadata.source_selection->scripted_request.instruction_payload_offset
                << "\n";
        }
    }
    if (metadata.expected_encounter.has_value()) {
        out << "  expected_encounter_source: "
            << battle_encounter_source_kind_name(
                metadata.expected_encounter->source_kind)
            << "\n";
        out << "  expected_encounter_id: "
            << metadata.expected_encounter->encounter_id << "\n";
    }
    out << "  turn_job_id: " << metadata.turn_job_id << "\n";
    if (metadata.exec_job_id.has_value()) {
        out << "  exec_job_id: " << *metadata.exec_job_id << "\n";
    }
    out << "  wave_id: " << metadata.wave_id << "\n";
    out << "  battle_set_id: " << metadata.battle_set_id << "\n";
    out << "  entry_savestate_id: " << metadata.entry_savestate_id << "\n";
    if (metadata.entry_savestate_sha256.has_value()) {
        out << "  entry_savestate_sha256: " << *metadata.entry_savestate_sha256 << "\n";
    }
    out << "  turn_index: " << metadata.turn_index << "\n";
    out << "  start_seed: " << metadata.starting_rng_seed << " ("
        << hex_seed(metadata.starting_rng_seed) << ")"
        << " source=" << battle_prediction_seed_source_name(metadata.seed_source) << "\n";
    out << "  start_boundary: "
        << battle_prediction_start_boundary_name(metadata.start_boundary) << "\n";
    out << "  fake_attacks: " << metadata.fake_attacks
        << " source=" << battle_prediction_fake_attack_source_name(metadata.fake_attack_source) << "\n";
    if (metadata.context_probe_id.has_value()) {
        out << "  context_probe_id: " << *metadata.context_probe_id
            << " source=" << battle_prediction_context_source_name(metadata.context_source);
        if (!metadata.context_status.empty()) {
            out << " status=" << metadata.context_status;
        }
        if (metadata.context_version.has_value()) {
            out << " version=" << *metadata.context_version;
        }
        out << "\n";
    }
    if (metadata.resolved_turn_variant_key.has_value()) {
        out << "  command_variant: " << *metadata.resolved_turn_variant_key << "\n";
    }
    for (const auto& warning : metadata.warnings) {
        out << "  warning: " << warning << "\n";
    }
}

void write_battle_prediction_run_json(
    const BattlePredictionDbInputMetadata& metadata,
    const BattlePredictionResult& result,
    std::ostream& out) {
    out << "{\n";
    out << "  \"input\": {\n";
    bool first = true;
    write_json_string_field(out, "db_root", metadata.source_db_root.generic_string(), first);
    write_json_string_field(out, "profile", metadata.profile_name, first);
    if (metadata.scenario_name.has_value()) {
        write_json_string_field(out, "scenario", *metadata.scenario_name, first);
    }
    if (metadata.source_selection.has_value()) {
        write_json_string_field(
            out,
            "source_producer",
            battle_source_producer_kind_name(
                metadata.source_selection->producer_kind),
            first);
        if (metadata.source_selection->producer_kind
            == BattleSourceProducerKind::ScriptedBattleRequest) {
            write_json_string_field(
                out,
                "source_script",
                metadata.source_selection->scripted_request.script_identity,
                first);
            write_json_string_field(
                out,
                "source_section",
                metadata.source_selection->scripted_request.section_identity,
                first);
            write_json_int_field(
                out,
                "source_payload_offset",
                metadata.source_selection->scripted_request.instruction_payload_offset,
                first);
        }
    }
    if (metadata.expected_encounter.has_value()) {
        write_json_string_field(
            out,
            "expected_encounter_source",
            battle_encounter_source_kind_name(
                metadata.expected_encounter->source_kind),
            first);
        write_json_int_field(
            out,
            "expected_encounter_id",
            metadata.expected_encounter->encounter_id,
            first);
    }
    if (metadata.requested_turn_job_id.has_value()) {
        write_json_i64_field(out, "requested_turn_job_id", *metadata.requested_turn_job_id, first);
    }
    if (metadata.requested_exec_job_id.has_value()) {
        write_json_i64_field(out, "requested_exec_job_id", *metadata.requested_exec_job_id, first);
    }
    write_json_i64_field(out, "turn_job_id", metadata.turn_job_id, first);
    if (metadata.exec_job_id.has_value()) {
        write_json_i64_field(out, "exec_job_id", *metadata.exec_job_id, first);
    }
    write_json_i64_field(out, "wave_id", metadata.wave_id, first);
    write_json_i64_field(out, "battle_set_id", metadata.battle_set_id, first);
    write_json_i64_field(out, "entry_savestate_id", metadata.entry_savestate_id, first);
    if (metadata.entry_savestate_sha256.has_value()) {
        write_json_string_field(
            out,
            "entry_savestate_sha256",
            *metadata.entry_savestate_sha256,
            first);
    }
    write_json_int_field(out, "turn_index", metadata.turn_index, first);
    if (metadata.seed_candidate_id.has_value()) {
        write_json_i64_field(out, "seed_candidate_id", *metadata.seed_candidate_id, first);
    }
    write_json_u32_field(out, "starting_rng_seed", metadata.starting_rng_seed, first);
    write_json_string_field(out, "starting_rng_seed_hex", hex_seed(metadata.starting_rng_seed), first);
    write_json_string_field(out, "seed_source", battle_prediction_seed_source_name(metadata.seed_source), first);
    write_json_string_field(
        out,
        "start_boundary",
        battle_prediction_start_boundary_name(metadata.start_boundary),
        first);
    write_json_int_field(out, "fake_attacks", metadata.fake_attacks, first);
    write_json_string_field(out, "fake_attack_source", battle_prediction_fake_attack_source_name(metadata.fake_attack_source), first);
    if (metadata.context_probe_id.has_value()) {
        write_json_i64_field(out, "context_probe_id", *metadata.context_probe_id, first);
        write_json_string_field(out, "context_source", battle_prediction_context_source_name(metadata.context_source), first);
    }
    if (metadata.context_version.has_value()) {
        write_json_int_field(out, "context_version", *metadata.context_version, first);
    }
    if (!metadata.context_status.empty()) {
        write_json_string_field(out, "context_status", metadata.context_status, first);
    }
    if (metadata.resolved_turn_variant_key.has_value()) {
        write_json_string_field(out, "resolved_turn_variant_key", *metadata.resolved_turn_variant_key, first);
    }
    if (!first) {
        out << ",\n";
    }
    out << "    \"warnings\": [";
    for (std::size_t i = 0; i < metadata.warnings.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << json_escape(metadata.warnings[i]) << "\"";
    }
    out << "]\n";
    out << "  },\n";
    out << "  \"prediction\": ";
    write_battle_prediction_json(result, out);
    out << "\n";
    out << "}\n";
}

} // namespace savor::predict
