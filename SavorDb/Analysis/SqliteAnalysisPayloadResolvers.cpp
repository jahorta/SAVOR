#include "SqliteAnalysisPayloadResolvers.h"

namespace savor::db::analysis {

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

std::optional<events::AnalysisSeedProbeResultPayloadView> SqliteSeedProbePayloadRowResolver::ResolveSeedProbeResult(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("probe_result", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id,probe_result_id FROM sp_probe_result WHERE probe_result_id=?1;",
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

    events::AnalysisSeedProbeResultPayloadView view{};
    view.probe_run_id = sqlite3_column_int64(st.st, 0);
    view.probe_result_id = sqlite3_column_int64(st.st, 1);
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

std::optional<events::AnalysisBattleTurnJobResultUpdatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleTurnJobResultUpdated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveBattleTurnJobRecorded(payload_ref_kind, payload_ref_id);
}

std::optional<events::AnalysisBattleTurnWaveStatusUpdatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleTurnWaveStatusUpdated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveBattleTurnWaveCreated(payload_ref_kind, payload_ref_id);
}

std::optional<events::AnalysisBattleBattleSetStatusUpdatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleSetStatusUpdated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveBattleSetCreated(payload_ref_kind, payload_ref_id);
}

std::optional<events::AnalysisBattleBattleAdvancementPoolCreatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleBattleAdvancementPoolCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("battle_advancement_pool", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_set_id, battle_advancement_pool_id FROM ab_battle_advancement_pool WHERE battle_advancement_pool_id=?1;",
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

    events::AnalysisBattleBattleAdvancementPoolCreatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.battle_advancement_pool_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisBattleBattleAdvancementDecisionRecordedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleBattleAdvancementDecisionRecorded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("battle_advancement_decision", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT p.battle_set_id, d.battle_advancement_pool_id, d.turn_job_id, d.battle_advancement_decision_id "
            "FROM ab_battle_advancement_decision d "
            "JOIN ab_battle_advancement_pool p ON p.battle_advancement_pool_id=d.battle_advancement_pool_id "
            "WHERE d.battle_advancement_decision_id=?1;",
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

    events::AnalysisBattleBattleAdvancementDecisionRecordedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.battle_advancement_pool_id = sqlite3_column_int64(st.st, 1);
    view.turn_job_id = sqlite3_column_int64(st.st, 2);
    view.battle_advancement_decision_id = sqlite3_column_int64(st.st, 3);
    return view;
}

std::optional<events::AnalysisBattleManualFollowupUpdatedPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleManualFollowupUpdated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("manual_followup", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT w.battle_set_id, f.turn_job_id, f.manual_followup_id "
            "FROM ab_manual_followup f "
            "JOIN ab_turn_job j ON j.turn_job_id=f.turn_job_id "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE f.manual_followup_id=?1;",
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

    events::AnalysisBattleManualFollowupUpdatedPayloadView view{};
    view.battle_set_id = sqlite3_column_int64(st.st, 0);
    view.turn_job_id = sqlite3_column_int64(st.st, 1);
    view.manual_followup_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::AnalysisBattleCompletionPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleCompletion(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("battle_completion", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_, "SELECT battle_completion_id FROM ab_battle_completion WHERE battle_completion_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return events::AnalysisBattleCompletionPayloadView{
        .battle_completion_id = sqlite3_column_int64(st.st, 0),
    };
}

std::optional<events::AnalysisBattleRecordingPayloadView> SqliteBattlePayloadRowResolver::ResolveBattleRecording(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("battle_recording", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_, "SELECT battle_completion_id,battle_recording_id FROM ab_battle_recording WHERE battle_recording_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return events::AnalysisBattleRecordingPayloadView{
        .battle_completion_id = sqlite3_column_int64(st.st, 0),
        .battle_recording_id = sqlite3_column_int64(st.st, 1),
    };
}

std::optional<events::AnalysisBattleReplayPayloadView>
SqliteBattlePayloadRowResolver::ResolveBattleReplay(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr ||
        !MatchesRef("battle_replay", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_, "SELECT battle_completion_id,battle_replay_id FROM ab_battle_replay WHERE battle_replay_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return events::AnalysisBattleReplayPayloadView{
        .battle_completion_id = sqlite3_column_int64(st.st, 0),
        .battle_replay_id = sqlite3_column_int64(st.st, 1),
    };
}

SqliteAnalysisSpinePayloadRowResolver::SqliteAnalysisSpinePayloadRowResolver(sqlite3* db)
    : db_(db) {
}

std::optional<events::AnalysisSpineRunCreatedPayloadView> SqliteAnalysisSpinePayloadRowResolver::ResolveSpineRunCreated(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("run", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT run_id FROM asp_run WHERE run_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::AnalysisSpineRunCreatedPayloadView view{};
    view.run_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::AnalysisSpineStateRefRegisteredPayloadView> SqliteAnalysisSpinePayloadRowResolver::ResolveSpineStateRefRegistered(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("state_ref", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT run_id, state_ref_id FROM asp_state_ref WHERE state_ref_id=?1;",
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

    events::AnalysisSpineStateRefRegisteredPayloadView view{};
    view.run_id = sqlite3_column_int64(st.st, 0);
    view.state_ref_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::AnalysisSpineLineageEdgeAddedPayloadView> SqliteAnalysisSpinePayloadRowResolver::ResolveSpineLineageEdgeAdded(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("lineage_edge", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT parent_run_id, child_run_id, lineage_edge_id "
            "FROM asp_lineage_edge WHERE lineage_edge_id=?1;",
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

    events::AnalysisSpineLineageEdgeAddedPayloadView view{};
    view.parent_run_id = sqlite3_column_int64(st.st, 0);
    view.child_run_id = sqlite3_column_int64(st.st, 1);
    view.lineage_edge_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::AnalysisSpineArtifactLinkedPayloadView> SqliteAnalysisSpinePayloadRowResolver::ResolveSpineArtifactLinked(
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || !MatchesRef("artifact_ref", payload_ref_kind, payload_ref_id)) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT run_id, artifact_ref_id, artifact_id "
            "FROM asp_artifact_ref WHERE artifact_ref_id=?1;",
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

    events::AnalysisSpineArtifactLinkedPayloadView view{};
    view.run_id = sqlite3_column_int64(st.st, 0);
    view.artifact_ref_id = sqlite3_column_int64(st.st, 1);
    view.artifact_id = sqlite3_column_int64(st.st, 2);
    return view;
}

} // namespace savor::db::analysis
