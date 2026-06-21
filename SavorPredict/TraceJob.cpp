#include "TraceJob.h"

#include "ProgressEventParser.h"
#include "RngModel.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace savor::predict {

namespace fs = std::filesystem;

namespace {

struct JobSnapshot {
    long long turn_job_id = 0;
    long long exec_job_id = 0;
    long long wave_id = 0;
    long long battle_set_id = 0;
    std::optional<int> rtc_value;
    int turn_index = 0;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
    std::uint32_t start_seed = 0;
    std::string start_seed_source;
    std::optional<std::uint32_t> end_seed;
    int battle_outcome = -1;
    std::string resolved_turn_commands_blob;
    std::string resolved_turn_variant_key;
    long long seed_candidate_id = 0;
    std::uint32_t seed_candidate_ordinal = 0;
    std::optional<int> seedprobe_delta;
};

struct CounterCandidateSummary {
    std::string actor;
    std::string target;
    int target_base_counter_chance = 0;
    bool target_base_counter_chance_known = false;
};

struct CritGateSummary {
    std::string actor;
    std::string target;
    int instr_param_0x6 = 0;
    bool instr_param_0x6_known = false;
    int attacker_base_agile = 0;
    bool attacker_base_agile_known = false;
    bool crit_draw_candidate = false;
    std::string note;
};

struct TraceSummary {
    JobSnapshot job;
    std::vector<std::string> command_summary;
    ParsedProgressEvents events;
    PreAiCameraDrawModel pre_ai_model;
    int pre_ai_draws = 0;
    std::vector<SoldierAiDecision> soldier_ai;
    int planned_soldier_attacks = 0;
    int observed_planned_soldier_attacks = 0;
    int observed_pc_attack_events = 0;
    int observed_soldier_attack_events = 0;
    int enemy_ai_draws = 0;
    TurnOrderSimulation turn_order;
    TurnOrderCheckpointExpectation turn_order_checkpoint_expectation;
    SoldierActionExecutionSummary soldier_action_execution;
    int turn_order_draws = 0;
    int known_through_turn_order = 0;
    int enemy_execution_setup_draws_if_all_planned_soldiers_act = 0;
    int confirmed_enemy_execution_setup_draws_from_observed_events = 0;
    int observed_shared_attack_damage_draw_floor = 0;
    int observed_shared_attack_optional_crit_candidates = 0;
    int observed_shared_attack_crit_skipped_by_instr_param_events = 0;
    AttackResolutionCheckpointExpectation attack_resolution_checkpoint_expectation;
    std::vector<CritGateSummary> crit_gate_events;
    int first_battle_soldier_drop_draws_from_events = 0;
    bool first_battle_soldier_drop_draws_known = true;
    std::optional<DropCheckpointExpectation> drop_checkpoint_expectation;
    OutcomeCheckpointExpectation outcome_checkpoint_expectation;
    ActionViewCameraExpectation action_view_camera_expectation;
    int first_battle_mode0e_camera_draws_for_observed_attacks = 0;
    int observed_lethal_attack_events = 0;
    int observed_nonlethal_attack_events = 0;
    int counter_draw_candidate_events = 0;
    CounterCheckpointExpectation counter_checkpoint_expectation;
    std::vector<CounterCandidateSummary> counter_candidates;
    int first_battle_status_attempt_draws_expected = 0;
    int post_turn_order_observed_draw_floor = 0;
    bool post_turn_order_observed_draw_floor_known = true;
    int post_turn_order_static_expected_observed_bucket = 0;
    bool post_turn_order_static_expected_observed_bucket_known = true;
    int post_turn_order_static_expected_with_crit_bucket = 0;
    bool post_turn_order_static_expected_with_crit_bucket_known = true;
    int post_turn_order_candidate_simple_roll_ceiling = 0;
    bool post_turn_order_candidate_simple_roll_ceiling_known = true;
    int modeled_observed_floor_through_post_turn_order = 0;
    int modeled_static_expected_observed_bucket_through_post_turn_order = 0;
    int modeled_static_expected_with_crit_bucket_through_post_turn_order = 0;
    int modeled_candidate_simple_roll_ceiling_through_post_turn_order = 0;
    std::optional<int> total_draw_distance;
};

class SqliteDb {
public:
    SqliteDb() = default;
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    ~SqliteDb() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    int open_readonly(const fs::path& path, std::ostream& err) {
        const int rc = sqlite3_open_v2(path.string().c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            err << "Failed to open " << path.string() << ": " << error() << "\n";
            return 1;
        }
        sqlite3_busy_timeout(db_, 5000);
        return 0;
    }

    sqlite3* get() const { return db_; }
    std::string error() const { return db_ == nullptr ? "sqlite error" : sqlite3_errmsg(db_); }

private:
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql, std::ostream& err) : db_(db) {
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            err << "Failed to prepare statement: " << sqlite3_errmsg(db_) << "\n" << sql << "\n";
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const { return stmt_; }
    bool ok() const { return stmt_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

std::uint32_t column_u32(sqlite3_stmt* stmt, int column) {
    return static_cast<std::uint32_t>(sqlite3_column_int64(stmt, column));
}

std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

int attach_database(sqlite3* db, const fs::path& path, const char* schema, std::ostream& err) {
    char* sql = sqlite3_mprintf("ATTACH DATABASE %Q AS %s", path.string().c_str(), schema);
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
        err << "Failed to attach " << path.string() << " as " << schema << ": "
            << (error == nullptr ? sqlite3_errmsg(db) : error) << "\n";
        sqlite3_free(error);
        return 1;
    }
    sqlite3_free(error);
    return 0;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            out << c;
            break;
        }
    }
    return out.str();
}

std::string hex_seed(std::uint32_t seed) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << seed;
    return out.str();
}

std::string join_ints(const std::vector<int>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

const char* outcome_name(int outcome) {
    switch (outcome) {
    case 0: return "Victory";
    case 1: return "Defeat";
    case 2: return "PredFailure";
    case 3: return "PlanMaterializeFailure";
    case 4: return "TurnsExhausted";
    case 5: return "DWRunErr";
    case 6: return "ReachedNextTurn";
    default: return "Unknown";
    }
}

std::string pc_slot_name(int slot) {
    switch (slot) {
    case 0: return "Vyse";
    case 1: return "Aika";
    default: return "PC" + std::to_string(slot);
    }
}

bool is_pc_actor(const std::string& actor) {
    return first_battle_is_pc_actor_name(actor);
}

bool is_soldier_actor(const std::string& actor) {
    return first_battle_is_soldier_actor_name(actor);
}

std::optional<int> first_battle_base_counter_chance_for_actor(const std::string& actor) {
    return first_battle_base_counter_chance_for_actor_name(actor);
}

std::optional<int> first_battle_base_agile_for_actor(const std::string& actor) {
    return first_battle_base_agile_for_actor_name(actor);
}

std::optional<int> soldier_slot_from_actor(const std::string& actor) {
    return first_battle_soldier_slot_from_progress_name(actor);
}

std::optional<int> first_battle_expected_instr_param_for_attack_actor(
    const std::string& actor,
    const std::vector<SoldierAiDecision>& soldier_ai) {
    if (is_pc_actor(actor)) {
        // First-battle PC basic attacks are expected to take the param-0 path
        // through HandlePCInst; live checkpoints still need to confirm it.
        return 0;
    }

    const auto soldier_slot = soldier_slot_from_actor(actor);
    if (!soldier_slot.has_value()) {
        return std::nullopt;
    }
    for (const auto& decision : soldier_ai) {
        if (decision.slot == *soldier_slot && decision.attack_param_rand.has_value()) {
            return soldier_attack_param_from_rand(*decision.attack_param_rand);
        }
    }
    return std::nullopt;
}

std::string action_kind_name(PlannedActionKind kind) {
    switch (kind) {
    case PlannedActionKind::Attack: return "Attack";
    case PlannedActionKind::Defend: return "Defend";
    case PlannedActionKind::Unknown:
    default:
        return "Unknown";
    }
}

int count_observed_planned_soldier_attacks(const ParsedProgressEvents& events) {
    if (!events.planned_actions.has_value()) {
        return 0;
    }
    int count = 0;
    if (events.planned_actions->soldier4.kind == PlannedActionKind::Attack) {
        ++count;
    }
    if (events.planned_actions->soldier5.kind == PlannedActionKind::Attack) {
        ++count;
    }
    return count;
}

void count_attack_events(
    const ParsedProgressEvents& events,
    int& pc_attack_events,
    int& soldier_attack_events) {
    pc_attack_events = 0;
    soldier_attack_events = 0;
    for (const auto& attack : events.attacks) {
        if (is_pc_actor(attack.actor)) {
            ++pc_attack_events;
        } else if (is_soldier_actor(attack.actor)) {
            ++soldier_attack_events;
        }
    }
}

int shared_attack_damage_draw_floor(const ParsedProgressEvents& events) {
    // A damage progress event proves the shared attack path reached at least
    // hit/dodge plus the two rollDamage calls. Optional crit/counter/status,
    // death/drop, and action-view camera draws are tracked separately.
    return static_cast<int>(events.attacks.size()) * 3;
}

std::optional<int> first_battle_soldier_drop_draws_for_item(const std::string& drop) {
    if (drop.find("Electri Box") != std::string::npos) {
        return 1;
    }
    if (drop.find("Moonberry") != std::string::npos) {
        return 2;
    }
    return std::nullopt;
}

void summarize_first_battle_drop_draws(
    const ParsedProgressEvents& events,
    int& drop_draws,
    bool& known) {
    drop_draws = 0;
    known = true;

    std::vector<bool> consumed_drops(events.drops.size(), false);
    for (const auto& death : events.deaths) {
        int matching_drop = -1;
        for (std::size_t i = 0; i < events.drops.size(); ++i) {
            if (!consumed_drops[i] && events.drops[i].target == death) {
                matching_drop = static_cast<int>(i);
                consumed_drops[i] = true;
                break;
            }
        }

        if (matching_drop < 0) {
            // First-battle Soldier has two enabled 1% rows; no drop means both
            // rows were evaluated and failed.
            drop_draws += 2;
            continue;
        }

        const auto item_draws = first_battle_soldier_drop_draws_for_item(events.drops[matching_drop].drop);
        if (!item_draws.has_value()) {
            known = false;
            continue;
        }
        drop_draws += *item_draws;
    }
}

void summarize_crit_gate_events(
    const ParsedProgressEvents& events,
    const std::vector<SoldierAiDecision>& soldier_ai,
    std::vector<CritGateSummary>& crit_gate_events,
    int& crit_candidates,
    int& crit_skipped_by_instr_param) {
    crit_gate_events.clear();
    crit_candidates = 0;
    crit_skipped_by_instr_param = 0;

    for (const auto& attack : events.attacks) {
        CritGateSummary gate;
        gate.actor = attack.actor;
        gate.target = attack.target;

        const auto instr_param = first_battle_expected_instr_param_for_attack_actor(attack.actor, soldier_ai);
        if (instr_param.has_value()) {
            gate.instr_param_0x6 = *instr_param;
            gate.instr_param_0x6_known = true;
        }

        const auto attacker_agile = first_battle_base_agile_for_actor(attack.actor);
        if (attacker_agile.has_value()) {
            gate.attacker_base_agile = *attacker_agile;
            gate.attacker_base_agile_known = true;
        }

        if (!gate.instr_param_0x6_known) {
            gate.crit_draw_candidate = true;
            gate.note = "instrParam_0x6 unknown; live checkpoint needed";
            ++crit_candidates;
        } else if (gate.instr_param_0x6 == 0) {
            gate.crit_draw_candidate = true;
            gate.note = "param 0 allows getAttackResult crit draw";
            ++crit_candidates;
        } else {
            gate.crit_draw_candidate = false;
            gate.note = "nonzero instrParam_0x6 skips getAttackResult crit draw";
            ++crit_skipped_by_instr_param;
        }

        crit_gate_events.push_back(std::move(gate));
    }
}

bool attack_is_followed_by_death_before_next_attack(
    const ParsedProgressEvents& events,
    std::size_t attack_event_index) {
    if (attack_event_index >= events.ordered_combat_events.size()) {
        return false;
    }

    const auto& attack = events.ordered_combat_events[attack_event_index].attack;
    for (std::size_t i = attack_event_index + 1; i < events.ordered_combat_events.size(); ++i) {
        const auto& event = events.ordered_combat_events[i];
        if (event.kind == CombatEventKind::Attack) {
            return false;
        }
        if (event.kind == CombatEventKind::Death && event.target == attack.target) {
            return true;
        }
    }
    return false;
}

void summarize_attack_lethality(
    const ParsedProgressEvents& events,
    int& lethal_attacks,
    int& nonlethal_attacks,
    std::vector<CounterCandidateSummary>& counter_candidates) {
    lethal_attacks = 0;
    nonlethal_attacks = 0;
    counter_candidates.clear();

    for (std::size_t i = 0; i < events.ordered_combat_events.size(); ++i) {
        if (events.ordered_combat_events[i].kind != CombatEventKind::Attack) {
            continue;
        }
        if (attack_is_followed_by_death_before_next_attack(events, i)) {
            ++lethal_attacks;
        } else {
            ++nonlethal_attacks;
            const auto& attack = events.ordered_combat_events[i].attack;
            CounterCandidateSummary candidate;
            candidate.actor = attack.actor;
            candidate.target = attack.target;
            const auto target_base_counter = first_battle_base_counter_chance_for_actor(attack.target);
            if (target_base_counter.has_value()) {
                candidate.target_base_counter_chance = *target_base_counter;
                candidate.target_base_counter_chance_known = true;
            }
            counter_candidates.push_back(std::move(candidate));
        }
    }
}

bool action_matches_decision(const PlannedAction& planned, const SoldierAiDecision& decision) {
    if (decision.attacks) {
        return planned.kind == PlannedActionKind::Attack;
    }
    return planned.kind == PlannedActionKind::Defend;
}

std::string target_match_text(const PlannedAction& planned, const SoldierAiDecision& decision) {
    if (!decision.attacks || !decision.target_pc_slot.has_value()) {
        return "n/a";
    }
    const auto expected = pc_slot_name(*decision.target_pc_slot);
    return planned.target.find(expected) != std::string::npos ? "ok" : "mismatch expected " + expected;
}

int read_job(sqlite3* db, const TraceJobOptions& options, JobSnapshot& job, std::ostream& err) {
    const char* sql = R"SQL(
        SELECT
            j.turn_job_id,
            j.exec_job_id,
            j.wave_id,
            w.battle_set_id,
            v.rtc_value,
            w.turn_index,
            j.fake_attacks_this_turn,
            j.fake_attacks_used_before,
            COALESCE(u.seed_value, sc.seed_value),
            CASE WHEN u.seed_value IS NOT NULL THEN 'sp_unique_seed.seed_value' ELSE 'ab_seed_candidate.seed_value' END,
            j.rng_seed,
            j.battle_outcome,
            COALESCE(j.resolved_turn_commands_blob, ''),
            COALESCE(j.resolved_turn_variant_key, ''),
            COALESCE(j.seed_candidate_id, w.seed_candidate_id),
            sc.seed_value,
            u.seed_delta
        FROM ab_turn_job j
        JOIN ab_turn_wave w ON w.wave_id = j.wave_id
        JOIN ab_battle_set bs ON bs.battle_set_id = w.battle_set_id
        LEFT JOIN st.state_tas_movie_variant v ON v.produced_savestate_id = bs.entry_savestate_id
        JOIN ab_seed_candidate sc ON sc.seed_candidate_id = COALESCE(j.seed_candidate_id, w.seed_candidate_id)
        LEFT JOIN sp_probe_run pr ON pr.entry_savestate_id = bs.entry_savestate_id
        LEFT JOIN sp_probe_result res ON res.probe_run_id = pr.probe_run_id
        LEFT JOIN sp_unique_seed u ON u.probe_result_id = res.probe_result_id
            AND u.input_frame_id = sc.source_input_frame_id
        WHERE (?1 IS NOT NULL AND j.turn_job_id = ?1)
           OR (?2 IS NOT NULL AND j.exec_job_id = ?2)
        LIMIT 1
    )SQL";

    Statement stmt(db, sql, err);
    if (!stmt.ok()) {
        return 1;
    }

    if (options.turn_job_id.has_value()) {
        sqlite3_bind_int64(stmt.get(), 1, *options.turn_job_id);
    } else {
        sqlite3_bind_null(stmt.get(), 1);
    }
    if (options.exec_job_id.has_value()) {
        sqlite3_bind_int64(stmt.get(), 2, *options.exec_job_id);
    } else {
        sqlite3_bind_null(stmt.get(), 2);
    }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        err << "No matching ab_turn_job found.\n";
        return 1;
    }
    if (rc != SQLITE_ROW) {
        err << "Failed to read ab_turn_job: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }

    job.turn_job_id = sqlite3_column_int64(stmt.get(), 0);
    job.exec_job_id = sqlite3_column_int64(stmt.get(), 1);
    job.wave_id = sqlite3_column_int64(stmt.get(), 2);
    job.battle_set_id = sqlite3_column_int64(stmt.get(), 3);
    if (sqlite3_column_type(stmt.get(), 4) != SQLITE_NULL) {
        job.rtc_value = sqlite3_column_int(stmt.get(), 4);
    }
    job.turn_index = sqlite3_column_int(stmt.get(), 5);
    job.fake_attacks_this_turn = sqlite3_column_int(stmt.get(), 6);
    job.fake_attacks_used_before = sqlite3_column_int(stmt.get(), 7);
    job.start_seed = column_u32(stmt.get(), 8);
    job.start_seed_source = column_text(stmt.get(), 9);
    if (sqlite3_column_type(stmt.get(), 10) != SQLITE_NULL) {
        job.end_seed = column_u32(stmt.get(), 10);
    }
    job.battle_outcome = sqlite3_column_int(stmt.get(), 11);
    job.resolved_turn_commands_blob = column_text(stmt.get(), 12);
    job.resolved_turn_variant_key = column_text(stmt.get(), 13);
    job.seed_candidate_id = sqlite3_column_int64(stmt.get(), 14);
    job.seed_candidate_ordinal = column_u32(stmt.get(), 15);
    if (sqlite3_column_type(stmt.get(), 16) != SQLITE_NULL) {
        job.seedprobe_delta = sqlite3_column_int(stmt.get(), 16);
    }
    return 0;
}

int read_progress_messages(sqlite3* db, long long exec_job_id, std::vector<std::string>& messages, std::ostream& err) {
    const char* sql = R"SQL(
        SELECT message
        FROM exec_job_event
        WHERE job_id = ?1
          AND event_kind = 'Execution.JobProgressed.v1'
          AND message IS NOT NULL
        ORDER BY job_event_id
    )SQL";

    Statement stmt(db, sql, err);
    if (!stmt.ok()) {
        return 1;
    }
    sqlite3_bind_int64(stmt.get(), 1, exec_job_id);

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            return 0;
        }
        if (rc != SQLITE_ROW) {
            err << "Failed to read exec_job_event rows: " << sqlite3_errmsg(db) << "\n";
            return 1;
        }
        messages.push_back(column_text(stmt.get(), 0));
    }
}

std::vector<std::string> summarize_commands(const std::string& commands_hex) {
    std::vector<std::string> lines;
    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(commands_hex);
    if (!commands.has_value()) {
        if (!commands_hex.empty()) {
            lines.push_back("unable to decode resolved turn commands");
        }
        return lines;
    }

    for (const auto& command : *commands) {
        std::ostringstream line;
        line << "slot " << static_cast<int>(command.actor_slot) << ": "
             << soa::battle::actions::get_action_string(command.macro);
        if (command.macro == soa::battle::actions::BattleAction::Attack) {
            line << " -> slot " << static_cast<int>(command.params.target_slot);
        }
        if (command.macro == soa::battle::actions::BattleAction::UseItem) {
            line << " item " << command.params.item_id
                 << " -> slot " << static_cast<int>(command.params.target_slot);
        }
        lines.push_back(line.str());
    }
    return lines;
}

TraceSummary build_trace(JobSnapshot job, ParsedProgressEvents events, std::vector<std::string> command_summary, int max_distance) {
    TraceSummary summary;
    summary.job = std::move(job);
    summary.events = std::move(events);
    summary.command_summary = std::move(command_summary);

    if (summary.job.end_seed.has_value()) {
        summary.total_draw_distance = bounded_distance(summary.job.start_seed, *summary.job.end_seed, max_distance);
    }

    summary.pre_ai_model = model_pre_ai_camera_draws(summary.job.fake_attacks_this_turn);
    summary.pre_ai_draws = summary.pre_ai_model.expected_total_draws;
    auto state = summary.job.start_seed;
    for (int i = 0; i < summary.pre_ai_draws; ++i) {
        state = advance_once(state);
    }

    for (int soldier_slot : {4, 5}) {
        summary.soldier_ai.push_back(resolve_soldier_ai(soldier_slot, state));
    }

    for (const auto& decision : summary.soldier_ai) {
        if (decision.attacks) {
            ++summary.planned_soldier_attacks;
        }
    }
    summary.observed_planned_soldier_attacks = count_observed_planned_soldier_attacks(summary.events);
    count_attack_events(summary.events, summary.observed_pc_attack_events, summary.observed_soldier_attack_events);

    summary.enemy_ai_draws = 2 + summary.planned_soldier_attacks * 2;
    const bool soldier4_attacks = summary.soldier_ai.size() > 0 && summary.soldier_ai[0].attacks;
    const bool soldier5_attacks = summary.soldier_ai.size() > 1 && summary.soldier_ai[1].attacks;
    summary.turn_order = simulate_turn_order(
        state,
        first_battle_basic_turn_order_entries(soldier4_attacks, soldier5_attacks));
    summary.turn_order_checkpoint_expectation = turn_order_checkpoint_expectation(summary.turn_order);
    summary.soldier_action_execution =
        analyze_first_battle_soldier_action_execution(summary.events, summary.turn_order);
    summary.turn_order_draws = summary.turn_order.draws_consumed;
    summary.known_through_turn_order = summary.pre_ai_draws + summary.enemy_ai_draws + summary.turn_order_draws;
    summary.enemy_execution_setup_draws_if_all_planned_soldiers_act = summary.planned_soldier_attacks;
    summary.confirmed_enemy_execution_setup_draws_from_observed_events =
        summary.soldier_action_execution.reached_execution_count;
    summary.observed_shared_attack_damage_draw_floor = shared_attack_damage_draw_floor(summary.events);
    summarize_crit_gate_events(
        summary.events,
        summary.soldier_ai,
        summary.crit_gate_events,
        summary.observed_shared_attack_optional_crit_candidates,
        summary.observed_shared_attack_crit_skipped_by_instr_param_events);
    summary.attack_resolution_checkpoint_expectation =
        first_battle_attack_resolution_checkpoint_expectation(
            summary.events,
            summary.observed_shared_attack_optional_crit_candidates);
    summarize_first_battle_drop_draws(
        summary.events,
        summary.first_battle_soldier_drop_draws_from_events,
        summary.first_battle_soldier_drop_draws_known);
    if (summary.first_battle_soldier_drop_draws_known) {
        summary.drop_checkpoint_expectation =
            first_battle_drop_checkpoint_expectation(summary.first_battle_soldier_drop_draws_from_events);
    }
    summary.outcome_checkpoint_expectation =
        first_battle_outcome_checkpoint_expectation(summary.job.battle_outcome);
    summary.action_view_camera_expectation = first_battle_action_view_camera_expectation(summary.events);
    summary.first_battle_mode0e_camera_draws_for_observed_attacks =
        summary.action_view_camera_expectation.expected_mode0e_camera_draws;
    summarize_attack_lethality(
        summary.events,
        summary.observed_lethal_attack_events,
        summary.observed_nonlethal_attack_events,
        summary.counter_candidates);
    summary.counter_draw_candidate_events = static_cast<int>(summary.counter_candidates.size());
    summary.counter_checkpoint_expectation =
        first_battle_counter_checkpoint_expectation(summary.counter_draw_candidate_events);
    summary.first_battle_status_attempt_draws_expected = 0;
    summary.post_turn_order_observed_draw_floor_known =
        summary.first_battle_soldier_drop_draws_known;
    summary.post_turn_order_observed_draw_floor =
        summary.confirmed_enemy_execution_setup_draws_from_observed_events
        + summary.observed_shared_attack_damage_draw_floor
        + summary.first_battle_status_attempt_draws_expected;
    if (summary.first_battle_soldier_drop_draws_known) {
        summary.post_turn_order_observed_draw_floor +=
            summary.first_battle_soldier_drop_draws_from_events;
    }
    summary.post_turn_order_static_expected_observed_bucket_known =
        summary.post_turn_order_observed_draw_floor_known;
    summary.post_turn_order_static_expected_observed_bucket =
        summary.post_turn_order_observed_draw_floor
        + summary.first_battle_mode0e_camera_draws_for_observed_attacks;
    summary.post_turn_order_static_expected_with_crit_bucket_known =
        summary.post_turn_order_static_expected_observed_bucket_known;
    summary.post_turn_order_static_expected_with_crit_bucket =
        summary.post_turn_order_static_expected_observed_bucket
        + summary.observed_shared_attack_optional_crit_candidates;
    summary.post_turn_order_candidate_simple_roll_ceiling_known =
        summary.post_turn_order_static_expected_with_crit_bucket_known;
    summary.post_turn_order_candidate_simple_roll_ceiling =
        summary.post_turn_order_static_expected_with_crit_bucket
        + summary.counter_draw_candidate_events;
    summary.modeled_observed_floor_through_post_turn_order =
        summary.known_through_turn_order + summary.post_turn_order_observed_draw_floor;
    summary.modeled_static_expected_observed_bucket_through_post_turn_order =
        summary.known_through_turn_order + summary.post_turn_order_static_expected_observed_bucket;
    summary.modeled_static_expected_with_crit_bucket_through_post_turn_order =
        summary.known_through_turn_order + summary.post_turn_order_static_expected_with_crit_bucket;
    summary.modeled_candidate_simple_roll_ceiling_through_post_turn_order =
        summary.known_through_turn_order + summary.post_turn_order_candidate_simple_roll_ceiling;
    return summary;
}

void write_text(const TraceSummary& summary, std::ostream& out) {
    out << "SavorPredict trace-job\n";
    out << "  turn_job_id: " << summary.job.turn_job_id << "\n";
    out << "  exec_job_id: " << summary.job.exec_job_id << "\n";
    out << "  battle_set_id: " << summary.job.battle_set_id << "\n";
    if (summary.job.rtc_value.has_value()) {
        out << "  rtc_value: " << *summary.job.rtc_value << "\n";
    }
    out << "  turn_index: " << summary.job.turn_index << "\n";
    out << "  fake_attacks_this_turn: " << summary.job.fake_attacks_this_turn << "\n";
    out << "  outcome: " << outcome_name(summary.job.battle_outcome) << " (" << summary.job.battle_outcome << ")\n";
    out << "  start_seed: " << summary.job.start_seed << " (" << hex_seed(summary.job.start_seed) << ")\n";
    out << "  start_seed_source: " << summary.job.start_seed_source << "\n";
    out << "  seed_candidate_ordinal: " << summary.job.seed_candidate_ordinal << "\n";
    if (summary.job.seedprobe_delta.has_value()) {
        out << "  seedprobe_delta: " << *summary.job.seedprobe_delta << "\n";
    }
    if (summary.job.end_seed.has_value()) {
        out << "  end_seed: " << *summary.job.end_seed << " (" << hex_seed(*summary.job.end_seed) << ")\n";
    } else {
        out << "  end_seed: null\n";
    }
    if (summary.total_draw_distance.has_value()) {
        out << "  total_lcg_draws_start_to_end: " << *summary.total_draw_distance << "\n";
    } else {
        out << "  total_lcg_draws_start_to_end: not found within bound\n";
    }

    if (!summary.command_summary.empty()) {
        out << "\nResolved player commands\n";
        for (const auto& command : summary.command_summary) {
            out << "  " << command << "\n";
        }
    }

    out << "\nObserved progress events\n";
    if (summary.events.planned_actions.has_value()) {
        const auto& plan = *summary.events.planned_actions;
        out << "  Vyse: " << planned_action_to_string(plan.vyse) << "\n";
        out << "  Aika: " << planned_action_to_string(plan.aika) << "\n";
        out << "  [4]Soldier: " << planned_action_to_string(plan.soldier4) << "\n";
        out << "  [5]Soldier: " << planned_action_to_string(plan.soldier5) << "\n";
    } else {
        out << "  planned actions: not found\n";
    }
    out << "  attacks: " << summary.events.attacks.size() << "\n";
    out << "    pc attack events: " << summary.observed_pc_attack_events << "\n";
    out << "    soldier attack events: " << summary.observed_soldier_attack_events << "\n";
    for (std::size_t i = 0; i < summary.events.attacks.size(); ++i) {
        const auto& attack = summary.events.attacks[i];
        out << "    #" << (i + 1) << " " << attack.actor << " -> "
            << attack.target << " damage=" << attack.damage << "\n";
    }
    out << "  deaths: " << summary.events.deaths.size() << "\n";
    for (const auto& death : summary.events.deaths) {
        out << "    " << death << "\n";
    }
    out << "  drops: " << summary.events.drops.size() << "\n";
    for (const auto& drop : summary.events.drops) {
        out << "    " << drop.target << " -> " << drop.drop << "\n";
    }
    if (summary.events.pc_before_ec_predicate.has_value()) {
        out << "  PCs act before ECs predicate: "
            << summary.events.pc_before_ec_predicate->lhs << " < "
            << summary.events.pc_before_ec_predicate->rhs << "\n";
    }

    out << "\nKnown RNG checkpoints\n";
    out << "  pre_ai_draws: " << summary.pre_ai_draws << "\n";
    out << "    rule: " << pre_ai_camera_rule_name(summary.pre_ai_model) << "\n";
    out << "    detail: " << pre_ai_camera_rule_detail(summary.pre_ai_model) << "\n";
    out << "    fake_attack_draws: " << summary.pre_ai_model.fake_attack_draws << "\n";
    out << "    baseline_camera_draws: " << summary.pre_ai_model.baseline_camera_draws << "\n";
    out << "    expected_camera_draws: " << summary.pre_ai_model.expected_camera_draws << "\n";
    out << "    suppressed_attack_targeting_camera_draws: "
        << summary.pre_ai_model.suppressed_attack_targeting_camera_draws << "\n";
    out << "    unsuppressed_fake_plus_camera_total: "
        << summary.pre_ai_model.unsuppressed_total_draws << "\n";
    out << "    first Soldier AI draw index: " << (summary.pre_ai_draws + 1) << " (1-based after battle start seed)\n";
    out << "    trace-checkpoints args: --expected-fake-attacks "
        << summary.pre_ai_model.fake_attacks << "\n";
    out << "  enemy_ai_draws: " << summary.enemy_ai_draws << "\n";
    out << "    planned Soldier attacks: " << summary.planned_soldier_attacks << "\n";
    if (summary.events.planned_actions.has_value()) {
        out << "    observed planned Soldier attacks: " << summary.observed_planned_soldier_attacks << "\n";
    }
    out << "  turn_order_draws: " << summary.turn_order_draws << "\n";
    out << "    queued_count: " << summary.turn_order.queued_count << "\n";
    out << "    sumQuick: " << summary.turn_order.sum_quick << "\n";
    out << "    jitter_modulus: " << summary.turn_order.jitter_modulus << "\n";
    out << "    execution_order_exact: "
        << (summary.turn_order.execution_order_exact ? "true" : "false") << "\n";
    if (!summary.turn_order.execution_slots.empty()) {
        out << "    execution_slots: " << join_ints(summary.turn_order.execution_slots) << "\n";
    }
    out << "    checkpoint expected priority-jitter draws: "
        << summary.turn_order_checkpoint_expectation.expected_priority_jitter_draws
        << " (" << summary.turn_order_checkpoint_expectation.pc << ")\n";
    out << "    checkpoint queued entries: "
        << summary.turn_order_checkpoint_expectation.expected_queued_entries
        << ", jitter_modulus: "
        << summary.turn_order_checkpoint_expectation.expected_jitter_modulus << "\n";
    out << "    checkpoint rule: " << turn_order_checkpoint_rule_detail() << "\n";
    out << "    trace-checkpoints args: --expected-turn-order-draws "
        << summary.turn_order_checkpoint_expectation.expected_priority_jitter_draws << "\n";
    out << "  known_through_turn_order: " << summary.known_through_turn_order << "\n";
    if (summary.total_draw_distance.has_value()) {
        out << "  residual_after_turn_order: " << (*summary.total_draw_distance - summary.known_through_turn_order) << "\n";
    }

    out << "\nPost-turn-order conditional checkpoints\n";
    out << "  enemy execution setup draw upper bound: "
        << summary.enemy_execution_setup_draws_if_all_planned_soldiers_act
        << " (HandleECInst:8008bc68 if each planned Soldier attack acts)\n";
    out << "  confirmed enemy setup draws from observed Soldier attack events: "
        << summary.confirmed_enemy_execution_setup_draws_from_observed_events << "\n";
    out << "    trace-checkpoints args: --expected-enemy-setup-draws "
        << summary.confirmed_enemy_execution_setup_draws_from_observed_events << "\n";
    out << "  Soldier action execution from progress order:\n";
    out << "    planned attacks: " << summary.soldier_action_execution.planned_attack_count << "\n";
    out << "    reached execution: " << summary.soldier_action_execution.reached_execution_count << "\n";
    out << "    death-prevented planned attacks: " << summary.soldier_action_execution.death_prevented_count << "\n";
    out << "    unresolved planned attacks: " << summary.soldier_action_execution.unresolved_planned_attack_count << "\n";
    for (const auto& soldier : summary.soldier_action_execution.soldiers) {
        out << "    [" << soldier.slot << "]Soldier status="
            << soldier_action_execution_status_name(soldier.status)
            << " planned=" << action_kind_name(soldier.planned_kind)
            << " turn_order_rank=";
        if (soldier.turn_order_rank.has_value()) {
            out << *soldier.turn_order_rank;
        } else {
            out << "unknown";
        }
        out << " attack_event_order=";
        if (soldier.attack_event_order.has_value()) {
            out << *soldier.attack_event_order;
        } else {
            out << "none";
        }
        out << " death_event_order=";
        if (soldier.death_event_order.has_value()) {
            out << *soldier.death_event_order;
        } else {
            out << "none";
        }
        out << "\n";
    }
    out << "  observed shared attack damage draw floor: "
        << summary.observed_shared_attack_damage_draw_floor
        << " (3 per damage event: hit/dodge plus two damage rolls)\n";
    out << "    checkpoint expectation: "
        << summary.attack_resolution_checkpoint_expectation.expected_hit_draws
        << " hit draws, "
        << summary.attack_resolution_checkpoint_expectation.expected_damage_spread_draws
        << " damage-spread draws, "
        << summary.attack_resolution_checkpoint_expectation.expected_damage_bonus_draws
        << " low-bit bonus draws\n";
    out << "    checkpoint rule: " << first_battle_attack_resolution_checkpoint_rule_detail() << "\n";
    out << "    trace-checkpoints args: --expected-attack-events "
        << summary.attack_resolution_checkpoint_expectation.observed_attack_events
        << " --expected-crit-draws "
        << summary.observed_shared_attack_optional_crit_candidates << "\n";
    out << "  damage formula simulation: helper implemented, exact prediction pending live current stats, HP, element, status, and draw-order checkpoints\n";
    out << "  optional crit draw candidates from damage events: "
        << summary.observed_shared_attack_optional_crit_candidates << "\n";
    out << "  crit draws skipped by known nonzero instrParam_0x6: "
        << summary.observed_shared_attack_crit_skipped_by_instr_param_events << "\n";
    for (std::size_t i = 0; i < summary.crit_gate_events.size(); ++i) {
        const auto& gate = summary.crit_gate_events[i];
        out << "    #" << (i + 1) << " " << gate.actor << " -> " << gate.target
            << " instrParam_0x6=";
        if (gate.instr_param_0x6_known) {
            out << gate.instr_param_0x6;
        } else {
            out << "unknown";
        }
        out << " attacker_base_agile=";
        if (gate.attacker_base_agile_known) {
            out << gate.attacker_base_agile;
        } else {
            out << "unknown";
        }
        out << " crit_candidate=" << (gate.crit_draw_candidate ? "yes" : "no")
            << " (" << gate.note << ")\n";
    }
    out << "  lethal observed attack events: " << summary.observed_lethal_attack_events << "\n";
    out << "  nonlethal observed attack events: " << summary.observed_nonlethal_attack_events << "\n";
    out << "  counter draw candidates from nonlethal events: "
        << summary.counter_draw_candidate_events
        << " (upper bound; crit/status/live counter gates still apply)\n";
    out << "    checkpoint ceiling: "
        << summary.counter_checkpoint_expectation.expected_counter_roll_ceiling
        << " (" << summary.counter_checkpoint_expectation.pc << ")\n";
    out << "    checkpoint rule: " << first_battle_counter_checkpoint_rule_detail() << "\n";
    out << "    trace-checkpoints args: --expected-counter-roll-ceiling "
        << summary.counter_checkpoint_expectation.expected_counter_roll_ceiling << "\n";
    for (std::size_t i = 0; i < summary.counter_candidates.size(); ++i) {
        const auto& candidate = summary.counter_candidates[i];
        out << "    #" << (i + 1) << " " << candidate.target
            << " can counter " << candidate.actor << " with base chance ";
        if (candidate.target_base_counter_chance_known) {
            out << candidate.target_base_counter_chance << "%";
        } else {
            out << "unknown";
        }
        out << " (live curCounterChance/status/crit gates pending)\n";
    }
    out << "  first-battle status-attempt draws expected: "
        << summary.first_battle_status_attempt_draws_expected
        << " (effect ids are -1 for first-battle sources)\n";
    out << "  first-battle Soldier drop draws from death/drop events: ";
    if (summary.first_battle_soldier_drop_draws_known) {
        out << summary.first_battle_soldier_drop_draws_from_events << "\n";
        if (summary.drop_checkpoint_expectation.has_value()) {
            out << "    checkpoint expected drop rolls: "
                << summary.drop_checkpoint_expectation->expected_drop_rolls
                << " (" << summary.drop_checkpoint_expectation->pc << ")\n";
            out << "    checkpoint rule: " << first_battle_drop_checkpoint_rule_detail() << "\n";
            out << "    trace-checkpoints args: --expected-drop-rolls "
                << summary.drop_checkpoint_expectation->expected_drop_rolls << "\n";
        }
    } else {
        out << "unknown item outside first-battle Soldier drop table\n";
    }
    out << "  outcome branch checkpoints:\n";
    out << "    rule: " << first_battle_outcome_checkpoint_rule_detail() << "\n";
    if (summary.outcome_checkpoint_expectation.expected_end_turn_status_draws.has_value()) {
        out << "    expected end-turn status cleanup draws: "
            << *summary.outcome_checkpoint_expectation.expected_end_turn_status_draws
            << " (" << summary.outcome_checkpoint_expectation.end_turn_pc << ")\n";
        out << "    trace-checkpoints args: --expected-end-turn-status-draws "
            << *summary.outcome_checkpoint_expectation.expected_end_turn_status_draws;
        if (summary.outcome_checkpoint_expectation.expected_level_up_stat_rolls.has_value()) {
            out << " --expected-level-up-stat-rolls "
                << *summary.outcome_checkpoint_expectation.expected_level_up_stat_rolls;
        }
        out << "\n";
    } else {
        out << "    exact end-turn/level-up expectations pending EXP and threshold modeling\n";
    }
    out << "  expected mode-0xe action-view camera draws for observed attacks: "
        << summary.first_battle_mode0e_camera_draws_for_observed_attacks
        << " (" << summary.action_view_camera_expectation.expected_pc
        << ", pending live field6/gate check)\n";
    out << "    rule: " << first_battle_action_view_camera_rule_detail() << "\n";
    out << "    rejected fallback owner: "
        << summary.action_view_camera_expectation.rejected_fallback_owner
        << " at " << summary.action_view_camera_expectation.rejected_fallback_pc << "\n";
    out << "  post-turn-order observed draw floor: ";
    if (summary.post_turn_order_observed_draw_floor_known) {
        out << summary.post_turn_order_observed_draw_floor << "\n";
        out << "    modeled through observed floor: "
            << summary.modeled_observed_floor_through_post_turn_order << "\n";
        if (summary.total_draw_distance.has_value()) {
            out << "    residual after observed floor: "
                << (*summary.total_draw_distance - summary.modeled_observed_floor_through_post_turn_order)
                << "\n";
        }
    } else {
        out << "unknown because a drop item was outside the first-battle Soldier table\n";
    }
    out << "  post-turn-order static expected observed bucket: ";
    if (summary.post_turn_order_static_expected_observed_bucket_known) {
        out << summary.post_turn_order_static_expected_observed_bucket
            << " (observed floor plus expected mode-0xe camera draws)\n";
        out << "    modeled through static expected bucket: "
            << summary.modeled_static_expected_observed_bucket_through_post_turn_order << "\n";
        if (summary.total_draw_distance.has_value()) {
            out << "    residual after static expected bucket: "
                << (*summary.total_draw_distance
                    - summary.modeled_static_expected_observed_bucket_through_post_turn_order)
                << "\n";
        }
    } else {
        out << "unknown because the observed floor is unknown\n";
    }
    out << "  post-turn-order static expected with crit bucket: ";
    if (summary.post_turn_order_static_expected_with_crit_bucket_known) {
        out << summary.post_turn_order_static_expected_with_crit_bucket
            << " (static expected bucket plus crit-gated draws)\n";
        out << "    modeled through static expected with crit bucket: "
            << summary.modeled_static_expected_with_crit_bucket_through_post_turn_order << "\n";
        if (summary.total_draw_distance.has_value()) {
            out << "    residual after static expected with crit bucket: "
                << (*summary.total_draw_distance
                    - summary.modeled_static_expected_with_crit_bucket_through_post_turn_order)
                << "\n";
        }
    } else {
        out << "unknown because the static expected bucket is unknown\n";
    }
    out << "  post-turn-order candidate simple-roll ceiling: ";
    if (summary.post_turn_order_candidate_simple_roll_ceiling_known) {
        out << summary.post_turn_order_candidate_simple_roll_ceiling
            << " (static expected with crit bucket plus counter candidates)\n";
        out << "    modeled through candidate simple-roll ceiling: "
            << summary.modeled_candidate_simple_roll_ceiling_through_post_turn_order << "\n";
        if (summary.total_draw_distance.has_value()) {
            out << "    residual after candidate simple-roll ceiling: "
                << (*summary.total_draw_distance
                    - summary.modeled_candidate_simple_roll_ceiling_through_post_turn_order)
                << "\n";
        }
    } else {
        out << "unknown because the static expected bucket is unknown\n";
    }
    out << "  note: aggregate events do not prove missed/interrupted Soldier turns or scheduler interleaving\n";
    out << "  note: floor excludes optional crit, live counter result, and exact death/drop interleaving\n";
    out << "  note: candidate simple-roll ceiling excludes counter follow-up attacks and later status-enabled branches\n";

    out << "\nSoldier AI model\n";
    for (const auto& decision : summary.soldier_ai) {
        const PlannedAction* observed = nullptr;
        if (summary.events.planned_actions.has_value()) {
            observed = decision.slot == 4 ? &summary.events.planned_actions->soldier4 : &summary.events.planned_actions->soldier5;
        }

        out << "  [" << decision.slot << "]Soldier action_rand=" << decision.action_rand
            << " mod100=" << (decision.action_rand % 100)
            << " -> " << (decision.attacks ? "Attack" : "Defend");
        if (decision.target_rand.has_value()) {
            out << " target_rand=" << *decision.target_rand
                << " target_pc=" << pc_slot_name(*decision.target_pc_slot);
        }
        if (decision.attack_param_rand.has_value()) {
            out << " attack_param_rand=" << *decision.attack_param_rand
                << " mod10=" << (*decision.attack_param_rand % 10)
                << " instrParam_0x6=" << soldier_attack_param_from_rand(*decision.attack_param_rand);
        }
        if (observed != nullptr) {
            out << " observed=" << action_kind_name(observed->kind)
                << " action_match=" << (action_matches_decision(*observed, decision) ? "ok" : "mismatch")
                << " target_match=" << target_match_text(*observed, decision);
        }
        out << "\n";
    }

    out << "\nGhidra anchors to verify next\n";
    out << "  8008a618 Battle::AI::runAiRoutine Soldier AI attack-parameter roll\n";
    out << "  8008bc68 Battle::HandleECInst enemy pre-attack roll\n";
    out << "  FUN_80087f6c / FUN_80087844 scheduled enemy attack worker paths\n";
    out << "  FUN_80012f58 / FUN_80009030 action-view aux-list suppression gate\n";
    out << "  FUN_80052b24:80052bf0 expected first-battle mode-0xe camera roll\n";
    out << "  setupTurnAction_80082134 -> performAttack_80081b94 shared hit/damage chain\n";
    out << "  enemyDropItem_8002ba8c death/drop roll for killed Soldiers\n";
}

void write_json(const TraceSummary& summary, std::ostream& out) {
    out << "{\n";
    out << "  \"turn_job_id\": " << summary.job.turn_job_id << ",\n";
    out << "  \"exec_job_id\": " << summary.job.exec_job_id << ",\n";
    out << "  \"battle_set_id\": " << summary.job.battle_set_id << ",\n";
    out << "  \"rtc_value\": ";
    if (summary.job.rtc_value.has_value()) {
        out << *summary.job.rtc_value;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"turn_index\": " << summary.job.turn_index << ",\n";
    out << "  \"fake_attacks_this_turn\": " << summary.job.fake_attacks_this_turn << ",\n";
    out << "  \"outcome\": \"" << outcome_name(summary.job.battle_outcome) << "\",\n";
    out << "  \"start_seed\": " << summary.job.start_seed << ",\n";
    out << "  \"start_seed_source\": \"" << json_escape(summary.job.start_seed_source) << "\",\n";
    out << "  \"seed_candidate_ordinal\": " << summary.job.seed_candidate_ordinal << ",\n";
    out << "  \"end_seed\": ";
    if (summary.job.end_seed.has_value()) {
        out << *summary.job.end_seed;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"total_lcg_draws_start_to_end\": ";
    if (summary.total_draw_distance.has_value()) {
        out << *summary.total_draw_distance;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"pre_ai_draws\": " << summary.pre_ai_draws << ",\n";
    out << "  \"pre_ai_rule\": \"" << pre_ai_camera_rule_name(summary.pre_ai_model) << "\",\n";
    out << "  \"pre_ai_rule_detail\": \"" << json_escape(pre_ai_camera_rule_detail(summary.pre_ai_model)) << "\",\n";
    out << "  \"pre_ai_fake_attack_draws\": " << summary.pre_ai_model.fake_attack_draws << ",\n";
    out << "  \"pre_ai_baseline_camera_draws\": " << summary.pre_ai_model.baseline_camera_draws << ",\n";
    out << "  \"pre_ai_expected_camera_draws\": " << summary.pre_ai_model.expected_camera_draws << ",\n";
    out << "  \"pre_ai_suppressed_attack_targeting_camera_draws\": "
        << summary.pre_ai_model.suppressed_attack_targeting_camera_draws << ",\n";
    out << "  \"pre_ai_unsuppressed_fake_plus_camera_total\": "
        << summary.pre_ai_model.unsuppressed_total_draws << ",\n";
    out << "  \"first_soldier_ai_draw_index_1_based\": " << (summary.pre_ai_draws + 1) << ",\n";
    out << "  \"enemy_ai_draws\": " << summary.enemy_ai_draws << ",\n";
    out << "  \"planned_soldier_attacks\": " << summary.planned_soldier_attacks << ",\n";
    out << "  \"observed_planned_soldier_attacks\": " << summary.observed_planned_soldier_attacks << ",\n";
    out << "  \"turn_order_draws\": " << summary.turn_order_draws << ",\n";
    out << "  \"turn_order_model\": {";
    out << "\"queued_count\": " << summary.turn_order.queued_count;
    out << ", \"sum_quick\": " << summary.turn_order.sum_quick;
    out << ", \"jitter_modulus\": " << summary.turn_order.jitter_modulus;
    out << ", \"priorities_complete\": " << (summary.turn_order.priorities_complete ? "true" : "false");
    out << ", \"priority_ties_ambiguous\": " << (summary.turn_order.priority_ties_ambiguous ? "true" : "false");
    out << ", \"execution_order_exact\": " << (summary.turn_order.execution_order_exact ? "true" : "false");
    out << ", \"execution_slots\": [";
    for (std::size_t i = 0; i < summary.turn_order.execution_slots.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << summary.turn_order.execution_slots[i];
    }
    out << "]},\n";
    out << "  \"turn_order_checkpoint_expectation\": {";
    out << "\"expected_priority_jitter_draws\": "
        << summary.turn_order_checkpoint_expectation.expected_priority_jitter_draws;
    out << ", \"expected_queued_entries\": "
        << summary.turn_order_checkpoint_expectation.expected_queued_entries;
    out << ", \"expected_jitter_modulus\": "
        << summary.turn_order_checkpoint_expectation.expected_jitter_modulus;
    out << ", \"owner\": \""
        << summary.turn_order_checkpoint_expectation.owner << "\"";
    out << ", \"pc\": \""
        << summary.turn_order_checkpoint_expectation.pc << "\"";
    out << ", \"rule\": \""
        << json_escape(turn_order_checkpoint_rule_detail()) << "\"";
    out << "},\n";
    out << "  \"known_through_turn_order\": " << summary.known_through_turn_order << ",\n";
    out << "  \"post_turn_order_model\": {";
    out << "\"enemy_execution_setup_draws_if_all_planned_soldiers_act\": "
        << summary.enemy_execution_setup_draws_if_all_planned_soldiers_act;
    out << ", \"confirmed_enemy_execution_setup_draws_from_observed_events\": "
        << summary.confirmed_enemy_execution_setup_draws_from_observed_events;
    out << ", \"soldier_action_execution\": {";
    out << "\"planned_attacks\": " << summary.soldier_action_execution.planned_attack_count;
    out << ", \"reached_execution\": " << summary.soldier_action_execution.reached_execution_count;
    out << ", \"death_prevented_planned_attacks\": " << summary.soldier_action_execution.death_prevented_count;
    out << ", \"unresolved_planned_attacks\": "
        << summary.soldier_action_execution.unresolved_planned_attack_count;
    out << ", \"soldiers\": [";
    for (std::size_t i = 0; i < summary.soldier_action_execution.soldiers.size(); ++i) {
        const auto& soldier = summary.soldier_action_execution.soldiers[i];
        if (i != 0) {
            out << ", ";
        }
        out << "{\"slot\": " << soldier.slot
            << ", \"planned\": \"" << action_kind_name(soldier.planned_kind)
            << "\", \"status\": \"" << soldier_action_execution_status_name(soldier.status)
            << "\", \"turn_order_rank\": ";
        if (soldier.turn_order_rank.has_value()) {
            out << *soldier.turn_order_rank;
        } else {
            out << "null";
        }
        out << ", \"attack_event_order\": ";
        if (soldier.attack_event_order.has_value()) {
            out << *soldier.attack_event_order;
        } else {
            out << "null";
        }
        out << ", \"death_event_order\": ";
        if (soldier.death_event_order.has_value()) {
            out << *soldier.death_event_order;
        } else {
            out << "null";
        }
        out << "}";
    }
    out << "]}";
    out << ", \"aggregate_limit\": \"progress events do not prove missed/interrupted Soldier turns or scheduler interleaving\"";
    out << "},\n";
    out << "  \"action_execution_observation\": {";
    out << "\"shared_attack_damage_draw_floor\": " << summary.observed_shared_attack_damage_draw_floor;
    out << ", \"shared_attack_damage_draw_floor_rule\": \"3 per damage event: hit/dodge plus two damage rolls\"";
    out << ", \"attack_resolution_checkpoint_expectation\": {";
    out << "\"expected_attack_events\": "
        << summary.attack_resolution_checkpoint_expectation.observed_attack_events;
    out << ", \"expected_hit_draws\": "
        << summary.attack_resolution_checkpoint_expectation.expected_hit_draws;
    out << ", \"expected_damage_spread_draws\": "
        << summary.attack_resolution_checkpoint_expectation.expected_damage_spread_draws;
    out << ", \"expected_damage_bonus_draws\": "
        << summary.attack_resolution_checkpoint_expectation.expected_damage_bonus_draws;
    out << ", \"expected_crit_draws\": ";
    if (summary.attack_resolution_checkpoint_expectation.expected_crit_draws.has_value()) {
        out << *summary.attack_resolution_checkpoint_expectation.expected_crit_draws;
    } else {
        out << "null";
    }
    out << ", \"rule\": \""
        << json_escape(first_battle_attack_resolution_checkpoint_rule_detail()) << "\"";
    out << "}";
    out << ", \"damage_formula_model_status\": \"helper implemented; exact prediction pending live current stats, HP, element, status, and draw-order checkpoints\"";
    out << ", \"optional_crit_draw_candidates\": " << summary.observed_shared_attack_optional_crit_candidates;
    out << ", \"crit_draws_skipped_by_known_nonzero_instr_param\": "
        << summary.observed_shared_attack_crit_skipped_by_instr_param_events;
    out << ", \"crit_gate_events\": [";
    for (std::size_t i = 0; i < summary.crit_gate_events.size(); ++i) {
        const auto& gate = summary.crit_gate_events[i];
        if (i != 0) {
            out << ", ";
        }
        out << "{\"actor\": \"" << json_escape(gate.actor)
            << "\", \"target\": \"" << json_escape(gate.target)
            << "\", \"instr_param_0x6\": ";
        if (gate.instr_param_0x6_known) {
            out << gate.instr_param_0x6;
        } else {
            out << "null";
        }
        out << ", \"instr_param_0x6_known\": "
            << (gate.instr_param_0x6_known ? "true" : "false")
            << ", \"attacker_base_agile\": ";
        if (gate.attacker_base_agile_known) {
            out << gate.attacker_base_agile;
        } else {
            out << "null";
        }
        out << ", \"attacker_base_agile_known\": "
            << (gate.attacker_base_agile_known ? "true" : "false")
            << ", \"crit_draw_candidate\": "
            << (gate.crit_draw_candidate ? "true" : "false")
            << ", \"note\": \"" << json_escape(gate.note) << "\"}";
    }
    out << "]";
    out << ", \"lethal_attack_events\": " << summary.observed_lethal_attack_events;
    out << ", \"nonlethal_attack_events\": " << summary.observed_nonlethal_attack_events;
    out << ", \"counter_draw_candidate_events\": " << summary.counter_draw_candidate_events;
    out << ", \"counter_checkpoint_expectation\": {";
    out << "\"expected_counter_roll_ceiling\": "
        << summary.counter_checkpoint_expectation.expected_counter_roll_ceiling;
    out << ", \"owner\": \"" << summary.counter_checkpoint_expectation.owner << "\"";
    out << ", \"pc\": \"" << summary.counter_checkpoint_expectation.pc << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_counter_checkpoint_rule_detail()) << "\"";
    out << "}";
    out << ", \"counter_draw_candidates\": [";
    for (std::size_t i = 0; i < summary.counter_candidates.size(); ++i) {
        const auto& candidate = summary.counter_candidates[i];
        if (i != 0) {
            out << ", ";
        }
        out << "{\"actor\": \"" << json_escape(candidate.actor)
            << "\", \"target\": \"" << json_escape(candidate.target)
            << "\", \"target_base_counter_chance\": ";
        if (candidate.target_base_counter_chance_known) {
            out << candidate.target_base_counter_chance;
        } else {
            out << "null";
        }
        out << ", \"target_base_counter_chance_known\": "
            << (candidate.target_base_counter_chance_known ? "true" : "false")
            << "}";
    }
    out << "]";
    out << ", \"first_battle_status_attempt_draws_expected\": "
        << summary.first_battle_status_attempt_draws_expected;
    out << ", \"first_battle_soldier_drop_draws_from_events\": ";
    if (summary.first_battle_soldier_drop_draws_known) {
        out << summary.first_battle_soldier_drop_draws_from_events;
    } else {
        out << "null";
    }
    out << ", \"first_battle_soldier_drop_draws_known\": "
        << (summary.first_battle_soldier_drop_draws_known ? "true" : "false");
    out << ", \"drop_checkpoint_expectation\": ";
    if (summary.drop_checkpoint_expectation.has_value()) {
        out << "{";
        out << "\"expected_drop_rolls\": "
            << summary.drop_checkpoint_expectation->expected_drop_rolls;
        out << ", \"owner\": \"" << summary.drop_checkpoint_expectation->owner << "\"";
        out << ", \"pc\": \"" << summary.drop_checkpoint_expectation->pc << "\"";
        out << ", \"rule\": \""
            << json_escape(first_battle_drop_checkpoint_rule_detail()) << "\"";
        out << "}";
    } else {
        out << "null";
    }
    out << ", \"outcome_checkpoint_expectation\": {";
    out << "\"expected_end_turn_status_draws\": ";
    if (summary.outcome_checkpoint_expectation.expected_end_turn_status_draws.has_value()) {
        out << *summary.outcome_checkpoint_expectation.expected_end_turn_status_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_level_up_stat_rolls\": ";
    if (summary.outcome_checkpoint_expectation.expected_level_up_stat_rolls.has_value()) {
        out << *summary.outcome_checkpoint_expectation.expected_level_up_stat_rolls;
    } else {
        out << "null";
    }
    out << ", \"end_turn_owner\": \""
        << summary.outcome_checkpoint_expectation.end_turn_owner << "\"";
    out << ", \"end_turn_pc\": \""
        << summary.outcome_checkpoint_expectation.end_turn_pc << "\"";
    out << ", \"level_up_roll_1_owner\": \""
        << summary.outcome_checkpoint_expectation.level_up_roll_1_owner << "\"";
    out << ", \"level_up_roll_2_owner\": \""
        << summary.outcome_checkpoint_expectation.level_up_roll_2_owner << "\"";
    out << ", \"level_up_roll_3_owner\": \""
        << summary.outcome_checkpoint_expectation.level_up_roll_3_owner << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_outcome_checkpoint_rule_detail()) << "\"";
    out << "}";
    out << ", \"mode0e_action_view_camera_draws_for_observed_attacks\": "
        << summary.first_battle_mode0e_camera_draws_for_observed_attacks;
    out << ", \"mode0e_action_view_camera_owner\": \""
        << summary.action_view_camera_expectation.expected_owner << "\"";
    out << ", \"mode0e_action_view_camera_pc\": \""
        << summary.action_view_camera_expectation.expected_pc << "\"";
    out << ", \"mode0e_action_view_camera_rule\": \""
        << json_escape(first_battle_action_view_camera_rule_detail()) << "\"";
    out << ", \"mode0e_action_view_camera_rejected_fallback_owner\": \""
        << summary.action_view_camera_expectation.rejected_fallback_owner << "\"";
    out << ", \"mode0e_action_view_camera_rejected_fallback_pc\": \""
        << summary.action_view_camera_expectation.rejected_fallback_pc << "\"";
    out << ", \"post_turn_order_observed_draw_floor\": ";
    if (summary.post_turn_order_observed_draw_floor_known) {
        out << summary.post_turn_order_observed_draw_floor;
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_observed_draw_floor_known\": "
        << (summary.post_turn_order_observed_draw_floor_known ? "true" : "false");
    out << ", \"modeled_observed_floor_through_post_turn_order\": ";
    if (summary.post_turn_order_observed_draw_floor_known) {
        out << summary.modeled_observed_floor_through_post_turn_order;
    } else {
        out << "null";
    }
    out << ", \"residual_after_observed_floor\": ";
    if (summary.total_draw_distance.has_value() && summary.post_turn_order_observed_draw_floor_known) {
        out << (*summary.total_draw_distance - summary.modeled_observed_floor_through_post_turn_order);
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_static_expected_observed_bucket\": ";
    if (summary.post_turn_order_static_expected_observed_bucket_known) {
        out << summary.post_turn_order_static_expected_observed_bucket;
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_static_expected_observed_bucket_known\": "
        << (summary.post_turn_order_static_expected_observed_bucket_known ? "true" : "false");
    out << ", \"modeled_static_expected_observed_bucket_through_post_turn_order\": ";
    if (summary.post_turn_order_static_expected_observed_bucket_known) {
        out << summary.modeled_static_expected_observed_bucket_through_post_turn_order;
    } else {
        out << "null";
    }
    out << ", \"residual_after_static_expected_observed_bucket\": ";
    if (summary.total_draw_distance.has_value()
        && summary.post_turn_order_static_expected_observed_bucket_known) {
        out << (*summary.total_draw_distance
            - summary.modeled_static_expected_observed_bucket_through_post_turn_order);
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_static_expected_with_crit_bucket\": ";
    if (summary.post_turn_order_static_expected_with_crit_bucket_known) {
        out << summary.post_turn_order_static_expected_with_crit_bucket;
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_static_expected_with_crit_bucket_known\": "
        << (summary.post_turn_order_static_expected_with_crit_bucket_known ? "true" : "false");
    out << ", \"modeled_static_expected_with_crit_bucket_through_post_turn_order\": ";
    if (summary.post_turn_order_static_expected_with_crit_bucket_known) {
        out << summary.modeled_static_expected_with_crit_bucket_through_post_turn_order;
    } else {
        out << "null";
    }
    out << ", \"residual_after_static_expected_with_crit_bucket\": ";
    if (summary.total_draw_distance.has_value()
        && summary.post_turn_order_static_expected_with_crit_bucket_known) {
        out << (*summary.total_draw_distance
            - summary.modeled_static_expected_with_crit_bucket_through_post_turn_order);
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_candidate_simple_roll_ceiling\": ";
    if (summary.post_turn_order_candidate_simple_roll_ceiling_known) {
        out << summary.post_turn_order_candidate_simple_roll_ceiling;
    } else {
        out << "null";
    }
    out << ", \"post_turn_order_candidate_simple_roll_ceiling_known\": "
        << (summary.post_turn_order_candidate_simple_roll_ceiling_known ? "true" : "false");
    out << ", \"modeled_candidate_simple_roll_ceiling_through_post_turn_order\": ";
    if (summary.post_turn_order_candidate_simple_roll_ceiling_known) {
        out << summary.modeled_candidate_simple_roll_ceiling_through_post_turn_order;
    } else {
        out << "null";
    }
    out << ", \"residual_after_candidate_simple_roll_ceiling\": ";
    if (summary.total_draw_distance.has_value()
        && summary.post_turn_order_candidate_simple_roll_ceiling_known) {
        out << (*summary.total_draw_distance
            - summary.modeled_candidate_simple_roll_ceiling_through_post_turn_order);
    } else {
        out << "null";
    }
    out << ", \"aggregate_limit\": \"camera expectation still needs live field6/gate/scheduler validation; crit candidates still need live queued-instruction and draw-index validation; counter candidates still need status/live counter gates; floor excludes optional crit, live counter result, and exact death/drop interleaving\"";
    out << "},\n";
    out << "  \"soldier_ai\": [\n";
    for (std::size_t i = 0; i < summary.soldier_ai.size(); ++i) {
        const auto& decision = summary.soldier_ai[i];
        out << "    {\"slot\": " << decision.slot
            << ", \"action_rand\": " << decision.action_rand
            << ", \"action_mod100\": " << (decision.action_rand % 100)
            << ", \"action\": \"" << (decision.attacks ? "Attack" : "Defend") << "\"";
        if (decision.target_pc_slot.has_value()) {
            out << ", \"target_pc\": \"" << pc_slot_name(*decision.target_pc_slot) << "\"";
            out << ", \"target_rand\": " << *decision.target_rand;
        }
        if (decision.attack_param_rand.has_value()) {
            out << ", \"attack_param_rand\": " << *decision.attack_param_rand;
            out << ", \"attack_param_mod10\": " << (*decision.attack_param_rand % 10);
            out << ", \"instr_param_0x6\": " << soldier_attack_param_from_rand(*decision.attack_param_rand);
        }
        out << "}";
        if (i + 1 != summary.soldier_ai.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"event_counts\": {\"attacks\": " << summary.events.attacks.size()
        << ", \"pc_attack_events\": " << summary.observed_pc_attack_events
        << ", \"soldier_attack_events\": " << summary.observed_soldier_attack_events
        << ", \"deaths\": " << summary.events.deaths.size()
        << ", \"drops\": " << summary.events.drops.size() << "},\n";
    out << "  \"resolved_turn_variant_key\": \"" << json_escape(summary.job.resolved_turn_variant_key) << "\"\n";
    out << "}\n";
}

} // namespace

int run_trace_job(const TraceJobOptions& options, std::ostream& out, std::ostream& err) {
    if (!options.turn_job_id.has_value() && !options.exec_job_id.has_value()) {
        err << "trace-job requires --turn-job-id or --exec-job-id.\n";
        return 2;
    }

    SqliteDb analysis_db;
    if (const int rc = analysis_db.open_readonly(options.db_root / "analysis.db", err); rc != 0) {
        return rc;
    }
    if (const int rc = attach_database(analysis_db.get(), options.db_root / "state.db", "st", err); rc != 0) {
        return rc;
    }

    SqliteDb execution_db;
    if (const int rc = execution_db.open_readonly(options.db_root / "execution.db", err); rc != 0) {
        return rc;
    }

    JobSnapshot job;
    if (const int rc = read_job(analysis_db.get(), options, job, err); rc != 0) {
        return rc;
    }

    std::vector<std::string> messages;
    if (const int rc = read_progress_messages(execution_db.get(), job.exec_job_id, messages, err); rc != 0) {
        return rc;
    }

    auto parsed_events = parse_progress_events(messages);
    auto command_summary = summarize_commands(job.resolved_turn_commands_blob);
    auto summary = build_trace(std::move(job), std::move(parsed_events), std::move(command_summary), options.max_distance);

    if (options.json) {
        write_json(summary, out);
    } else {
        write_text(summary, out);
    }
    return 0;
}

} // namespace savor::predict
