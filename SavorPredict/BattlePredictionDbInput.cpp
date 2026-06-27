#include "BattlePredictionDbInput.h"

#include "BattleJobRunOptions.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include <Analysis/SqliteAnalysisDb.h>

#include <sqlite3.h>

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

std::optional<savor::db::BattleTurnJobSnapshot> resolve_turn_job(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionJobSelector& selector,
    std::ostream& err) {
    if (selector.turn_job_id.has_value() == selector.exec_job_id.has_value()) {
        err << "Specify exactly one DB job selector.\n";
        return std::nullopt;
    }
    if (selector.turn_job_id.has_value()) {
        auto row = analysis_db.GetBattleTurnJob(*selector.turn_job_id);
        if (!row.has_value()) {
            err << "No matching ab_turn_job found for turn_job_id " << *selector.turn_job_id << ".\n";
        }
        return row;
    }

    auto row = analysis_db.GetBattleTurnJobForExecJob(*selector.exec_job_id);
    if (!row.has_value()) {
        err << "No matching ab_turn_job found for exec_job_id " << *selector.exec_job_id << ".\n";
    }
    return row;
}

std::optional<savor::db::BattleContextProbeSnapshot> resolve_context_probe(
    const savor::db::IAnalysisDb& analysis_db,
    const savor::db::BattleTurnWaveSnapshot& wave,
    BattlePredictionDbInputMetadata& metadata) {
    if (wave.context_probe_id.has_value()) {
        auto probe = analysis_db.GetBattleContextProbe(*wave.context_probe_id);
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
    if (probe.has_value()) {
        metadata.context_source = BattlePredictionContextSource::LatestWaveContextProbe;
    }
    return probe;
}

} // namespace

const char* battle_prediction_seed_source_name(BattlePredictionSeedSource source) {
    switch (source) {
    case BattlePredictionSeedSource::Override: return "override";
    case BattlePredictionSeedSource::SeedProbeUniqueSeed: return "sp_unique_seed.seed_value";
    case BattlePredictionSeedSource::SeedCandidate: return "ab_seed_candidate.seed_value";
    case BattlePredictionSeedSource::SeedCandidateFallback: return "seed_candidate_fallback";
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

std::optional<BattlePredictionDbInput> build_battle_prediction_input_from_analysis_db(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionDbInputOptions& options,
    std::ostream& err) {
    const auto profile = battle_prediction_profile_by_name(options.profile_name);
    if (!profile.has_value()) {
        err << "Unsupported profile: " << options.profile_name << "\n";
        return std::nullopt;
    }

    auto turn_job = resolve_turn_job(analysis_db, options.selector, err);
    if (!turn_job.has_value()) {
        return std::nullopt;
    }

    const auto wave = analysis_db.GetBattleTurnWave(turn_job->wave_id);
    if (!wave.has_value()) {
        err << "Selected turn job references missing wave " << turn_job->wave_id << ".\n";
        return std::nullopt;
    }
    const auto battle_set = analysis_db.GetBattleSet(wave->battle_set_id);
    if (!battle_set.has_value()) {
        err << "Selected turn job references missing battle set " << wave->battle_set_id << ".\n";
        return std::nullopt;
    }

    const auto seed_candidate_id = turn_job->seed_candidate_id.value_or(wave->seed_candidate_id);
    const auto seed_candidate = analysis_db.GetBattleSeedCandidate(seed_candidate_id);

    BattlePredictionDbInput resolved;
    resolved.input.profile = *profile;
    resolved.input.options.movement_backend = options.movement_backend;
    resolved.input.options.action_view_std_json_dir = options.action_view_std_json_dir;
    auto& metadata = resolved.metadata;
    metadata.source_db_root = options.db_root;
    metadata.requested_turn_job_id = options.selector.turn_job_id;
    metadata.requested_exec_job_id = options.selector.exec_job_id;
    metadata.turn_job_id = turn_job->turn_job_id;
    metadata.exec_job_id = turn_job->exec_job_id;
    metadata.wave_id = wave->wave_id;
    metadata.battle_set_id = wave->battle_set_id;
    metadata.turn_index = wave->turn_index;
    metadata.seed_candidate_id = seed_candidate_id;
    metadata.resolved_turn_variant_key = turn_job->resolved_turn_variant_key;

    if (options.start_seed_override.has_value()) {
        resolved.input.starting_rng_seed = *options.start_seed_override;
        metadata.seed_source = BattlePredictionSeedSource::Override;
        if (turn_job->rng_seed.has_value()) {
            metadata.warnings.push_back("start seed override ignored stored turn-job RNG seed");
        }
    } else {
        if (!seed_candidate.has_value()) {
            err << "Selected turn job references seed candidate "
                << seed_candidate_id << " could not be loaded.\n";
            return std::nullopt;
        }

        if (seed_candidate->source_unique_seed_id.has_value()) {
            const auto unique_seed =
                analysis_db.GetSeedProbeUniqueSeed(*seed_candidate->source_unique_seed_id);
            if (unique_seed.has_value()) {
                resolved.input.starting_rng_seed = checked_seed_from_i64(unique_seed->seed_value);
                metadata.seed_source = BattlePredictionSeedSource::SeedProbeUniqueSeed;
            } else if (options.allow_seed_candidate_fallback) {
                resolved.input.starting_rng_seed = checked_seed_from_i64(seed_candidate->seed_value);
                metadata.seed_source = BattlePredictionSeedSource::SeedCandidateFallback;
                metadata.warnings.push_back(
                    "linked sp_unique_seed row was missing; using ab_seed_candidate.seed_value fallback");
            } else {
                err << "Selected seed candidate references missing sp_unique_seed "
                    << *seed_candidate->source_unique_seed_id
                    << "; rerun with --allow-seed-candidate-fallback to use ab_seed_candidate.seed_value.\n";
                return std::nullopt;
            }
        } else if (seed_candidate->source_input_frame_id.has_value()) {
            const auto unique_seed =
                analysis_db.FindSeedProbeUniqueSeedForEntrySavestateInputFrame(
                    battle_set->entry_savestate_id,
                    *seed_candidate->source_input_frame_id);
            if (unique_seed.has_value()) {
                resolved.input.starting_rng_seed = checked_seed_from_i64(unique_seed->seed_value);
                metadata.seed_source = BattlePredictionSeedSource::SeedProbeUniqueSeed;
            } else if (options.allow_seed_candidate_fallback) {
                resolved.input.starting_rng_seed = checked_seed_from_i64(seed_candidate->seed_value);
                metadata.seed_source = BattlePredictionSeedSource::SeedCandidateFallback;
                metadata.warnings.push_back(
                    "no sp_unique_seed matched seed candidate source input frame; using ab_seed_candidate.seed_value fallback");
            } else {
                err << "Selected seed candidate has source input frame "
                    << *seed_candidate->source_input_frame_id
                    << " but no matching sp_unique_seed for battle set entry savestate "
                    << battle_set->entry_savestate_id
                    << "; rerun with --allow-seed-candidate-fallback to use ab_seed_candidate.seed_value.\n";
                return std::nullopt;
            }
        } else {
            resolved.input.starting_rng_seed = checked_seed_from_i64(seed_candidate->seed_value);
            metadata.seed_source = BattlePredictionSeedSource::SeedCandidate;
            if (turn_job->rng_seed.has_value()) {
                metadata.warnings.push_back(
                    "using ab_seed_candidate.seed_value as battle start seed; stored turn-job RNG seed is not used for predictor input");
            }
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
    resolved.input.enemy_event_id = options.enemy_event_id;
    metadata.enemy_event_id = options.enemy_event_id;

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

    const auto probe = resolve_context_probe(analysis_db, *wave, metadata);
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
    return build_battle_prediction_input_from_analysis_db(analysis_db, options, err);
}

void write_battle_prediction_db_metadata_text(
    const BattlePredictionDbInputMetadata& metadata,
    std::ostream& out) {
    out << "DB-backed prediction input\n";
    out << "  db_root: " << metadata.source_db_root.string() << "\n";
    out << "  turn_job_id: " << metadata.turn_job_id << "\n";
    if (metadata.exec_job_id.has_value()) {
        out << "  exec_job_id: " << *metadata.exec_job_id << "\n";
    }
    out << "  wave_id: " << metadata.wave_id << "\n";
    out << "  battle_set_id: " << metadata.battle_set_id << "\n";
    out << "  turn_index: " << metadata.turn_index << "\n";
    out << "  start_seed: " << metadata.starting_rng_seed << " ("
        << hex_seed(metadata.starting_rng_seed) << ")"
        << " source=" << battle_prediction_seed_source_name(metadata.seed_source) << "\n";
    out << "  fake_attacks: " << metadata.fake_attacks
        << " source=" << battle_prediction_fake_attack_source_name(metadata.fake_attack_source) << "\n";
    if (metadata.enemy_event_id.has_value()) {
        out << "  enemy_event_id: " << *metadata.enemy_event_id << "\n";
    }
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
    write_json_int_field(out, "turn_index", metadata.turn_index, first);
    if (metadata.seed_candidate_id.has_value()) {
        write_json_i64_field(out, "seed_candidate_id", *metadata.seed_candidate_id, first);
    }
    write_json_u32_field(out, "starting_rng_seed", metadata.starting_rng_seed, first);
    write_json_string_field(out, "starting_rng_seed_hex", hex_seed(metadata.starting_rng_seed), first);
    write_json_string_field(out, "seed_source", battle_prediction_seed_source_name(metadata.seed_source), first);
    write_json_int_field(out, "fake_attacks", metadata.fake_attacks, first);
    write_json_string_field(out, "fake_attack_source", battle_prediction_fake_attack_source_name(metadata.fake_attack_source), first);
    if (metadata.enemy_event_id.has_value()) {
        write_json_int_field(out, "enemy_event_id", *metadata.enemy_event_id, first);
    }
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
