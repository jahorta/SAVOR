#include "SqliteAnalysisPayloadResolvers.h"

namespace simcore::db::analysis {

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool MatchesRef(std::string_view expected_kind, std::string_view payload_ref_kind, std::int64_t payload_ref_id) {
    return payload_ref_kind == expected_kind && payload_ref_id > 0;
}

} // namespace

SqliteSeedProbePayloadRowResolver::SqliteSeedProbePayloadRowResolver(sqlite3* db)
    : db_(db) {
}

std::optional<events::AnalysisSeedProbeSetCreatedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeSetCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("probe_set", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT probe_set_id FROM sp_probe_set WHERE probe_set_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeSetCreatedPayloadView view{};
    view.probe_set_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::AnalysisSeedProbeRunRequestedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeRunRequested(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("probe_run", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT probe_set_id, probe_run_id FROM sp_probe_run WHERE probe_run_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeRunRequestedPayloadView view{};
    view.probe_set_id = sqlite3_column_int64(st.st, 0);
    view.probe_run_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisSeedProbeNeutralSeedRecordedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeNeutralSeedRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("neutral_seed", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT pr.probe_run_id, n.probe_result_id, n.neutral_seed_id "
            "FROM sp_neutral_seed n "
            "JOIN sp_probe_result pr ON pr.probe_result_id=n.probe_result_id "
            "WHERE n.neutral_seed_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeNeutralSeedRecordedPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.probe_result_id = sqlite3_column_int64(st.st, 1);
    view.neutral_seed_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::AnalysisSeedProbeGridSeedRecordedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeGridSeedRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("grid_seed", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT pr.probe_run_id, g.probe_result_id, g.grid_seed_id "
            "FROM sp_grid_seed g "
            "JOIN sp_probe_result pr ON pr.probe_result_id=g.probe_result_id "
            "WHERE g.grid_seed_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeGridSeedRecordedPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.probe_result_id = sqlite3_column_int64(st.st, 1);
    view.grid_seed_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::AnalysisSeedProbeUniqueSeedRecordedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeUniqueSeedRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("unique_seed", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT pr.probe_run_id, u.probe_result_id, u.unique_seed_id "
            "FROM sp_unique_seed u "
            "JOIN sp_probe_result pr ON pr.probe_result_id=u.probe_result_id "
            "WHERE u.unique_seed_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeUniqueSeedRecordedPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.probe_result_id = sqlite3_column_int64(st.st, 1);
    view.unique_seed_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::AnalysisSeedProbeEncounterProjectionRecordedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeEncounterProjectionRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("encounter_projection", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id, encounter_projection_id "
            "FROM sp_encounter_projection WHERE encounter_projection_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeEncounterProjectionRecordedPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.encounter_projection_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisSeedProbeRunCompletedPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeRunCompleted(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("probe_result", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id, probe_result_id FROM sp_probe_result WHERE probe_result_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSeedProbeRunCompletedPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.probe_result_id = sqlite3_column_int64(st.st, 1);
    return view;
}

SqliteBattlePayloadRowResolver::SqliteBattlePayloadRowResolver(sqlite3* db)
    : db_(db) {
}

std::optional<events::AnalysisBattleSetCreatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleSetCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("battle_set", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT battle_set_id FROM ab_battle_set WHERE battle_set_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleSetCreatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::AnalysisBattleSeedCandidateAddedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleSeedCandidateAdded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("seed_candidate", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_set_id, seed_candidate_id FROM ab_seed_candidate WHERE seed_candidate_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleSeedCandidateAddedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.seed_candidate_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisBattleTurnWaveCreatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleTurnWaveCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("turn_wave", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_set_id, wave_id FROM ab_turn_wave WHERE wave_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleTurnWaveCreatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.wave_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisBattleTurnJobRecordedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleTurnJobRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("turn_job", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT w.battle_set_id, j.wave_id, j.turn_job_id, COALESCE(j.exec_job_id, 0) "
            "FROM ab_turn_job j "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE j.turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleTurnJobRecordedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.wave_id = sqlite3_column_int64(st.st, 1);
    view.turn_job_id = sqlite3_column_int64(st.st, 2);
    view.exec_job_id = sqlite3_column_int64(st.st, 3);
    return view;
}

std::optional<events::AnalysisBattleSelectionPoolCreatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleSelectionPoolCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("selection_pool", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_set_id, selection_pool_id FROM ab_selection_pool WHERE selection_pool_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleSelectionPoolCreatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.selection_pool_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisBattleSelectionDecisionRecordedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleSelectionDecisionRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("selection_decision", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.battle_set_id, d.selection_pool_id, d.turn_job_id, d.selection_decision_id "
            "FROM ab_selection_decision d "
            "JOIN ab_selection_pool p ON p.selection_pool_id=d.selection_pool_id "
            "WHERE d.selection_decision_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleSelectionDecisionRecordedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.selection_pool_id = sqlite3_column_int64(st.st, 1);
    view.turn_job_id = sqlite3_column_int64(st.st, 2);
    view.selection_decision_id = sqlite3_column_int64(st.st, 3);
    return view;
}

std::optional<events::AnalysisBattleTerminalFollowupUpdatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleTerminalFollowupUpdated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("terminal_followup", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT w.battle_set_id, f.turn_job_id, f.terminal_followup_id "
            "FROM ab_terminal_followup f "
            "JOIN ab_turn_job j ON j.turn_job_id=f.turn_job_id "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE f.terminal_followup_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisBattleTerminalFollowupUpdatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.turn_job_id = sqlite3_column_int64(st.st, 1);
    view.terminal_followup_id = sqlite3_column_int64(st.st, 2);
    return view;
}

} // namespace simcore::db::analysis
