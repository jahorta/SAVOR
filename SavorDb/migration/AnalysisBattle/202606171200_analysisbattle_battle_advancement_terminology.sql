BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = OFF;

DROP INDEX IF EXISTS uq_ab_selection_pool_battle_turn_name;
DROP INDEX IF EXISTS uq_ab_selection_decision_pool_turn_job;
DROP INDEX IF EXISTS ix_ab_selection_decision_pool_kind;
DROP INDEX IF EXISTS ix_ab_terminal_followup_status_victory;

ALTER TABLE ab_selection_pool RENAME TO ab_battle_advancement_pool;
ALTER TABLE ab_battle_advancement_pool RENAME COLUMN selection_pool_id TO battle_advancement_pool_id;
ALTER TABLE ab_turn_wave RENAME COLUMN selection_pool_id TO battle_advancement_pool_id;

CREATE TABLE ab_battle_advancement_decision_new (
    battle_advancement_decision_id INTEGER PRIMARY KEY,
    battle_advancement_pool_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    decision_kind TEXT NOT NULL CHECK(decision_kind IN ('SELECTED', 'NOT_SELECTED', 'REJECTED')),
    decision_reason TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_advancement_pool_id) REFERENCES ab_battle_advancement_pool(battle_advancement_pool_id),
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

INSERT INTO ab_battle_advancement_decision_new(
    battle_advancement_decision_id,
    battle_advancement_pool_id,
    turn_job_id,
    decision_kind,
    decision_reason,
    created_at_utc)
SELECT
    selection_decision_id,
    selection_pool_id,
    turn_job_id,
    CASE decision_kind
        WHEN 'WINNER' THEN 'SELECTED'
        WHEN 'DUPLICATE' THEN 'NOT_SELECTED'
        ELSE decision_kind
    END,
    decision_reason,
    created_at_utc
FROM ab_selection_decision;

DROP TABLE ab_selection_decision;
ALTER TABLE ab_battle_advancement_decision_new RENAME TO ab_battle_advancement_decision;

CREATE TABLE ab_manual_followup_new (
    manual_followup_id INTEGER PRIMARY KEY,
    turn_job_id INTEGER NOT NULL,
    manual_followup_status TEXT NOT NULL DEFAULT 'UNREVIEWED' CHECK(manual_followup_status IN ('UNREVIEWED', 'RECORDED')),
    recorded_dtm_artifact_id INTEGER NULL,
    recorded_dtmini_artifact_id INTEGER NULL,
    recorded_sav_artifact_id INTEGER NULL,
    note TEXT NULL,
    updated_at_utc INTEGER NOT NULL,
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id),
    CONSTRAINT uq_ab_manual_followup_turn_job UNIQUE (turn_job_id),
    CONSTRAINT ck_ab_manual_followup_recorded_artifact
        CHECK(manual_followup_status <> 'RECORDED' OR recorded_dtm_artifact_id IS NOT NULL)
);

INSERT INTO ab_manual_followup_new(
    manual_followup_id,
    turn_job_id,
    manual_followup_status,
    recorded_dtm_artifact_id,
    recorded_dtmini_artifact_id,
    recorded_sav_artifact_id,
    note,
    updated_at_utc)
SELECT
    terminal_followup_id,
    turn_job_id,
    manual_followup_status,
    recorded_dtm_artifact_id,
    recorded_dtmini_artifact_id,
    recorded_sav_artifact_id,
    note,
    updated_at_utc
FROM ab_terminal_followup;

DROP TABLE ab_terminal_followup;
ALTER TABLE ab_manual_followup_new RENAME TO ab_manual_followup;

UPDATE ab_outbox_message
SET event_type = CASE event_type
        WHEN 'AnalysisBattle.SelectionPoolCreated.v1' THEN 'AnalysisBattle.BattleAdvancementPoolCreated.v1'
        WHEN 'AnalysisBattle.SelectionDecisionRecorded.v1' THEN 'AnalysisBattle.BattleAdvancementDecisionRecorded.v1'
        WHEN 'AnalysisBattle.TerminalFollowupUpdated.v1' THEN 'AnalysisBattle.ManualFollowupUpdated.v1'
        ELSE event_type
    END,
    payload_ref_kind = CASE payload_ref_kind
        WHEN 'selection_pool' THEN 'battle_advancement_pool'
        WHEN 'selection_decision' THEN 'battle_advancement_decision'
        WHEN 'terminal_followup' THEN 'manual_followup'
        ELSE payload_ref_kind
    END
WHERE event_type IN (
        'AnalysisBattle.SelectionPoolCreated.v1',
        'AnalysisBattle.SelectionDecisionRecorded.v1',
        'AnalysisBattle.TerminalFollowupUpdated.v1')
   OR payload_ref_kind IN ('selection_pool', 'selection_decision', 'terminal_followup');

CREATE UNIQUE INDEX IF NOT EXISTS uq_ab_battle_advancement_pool_battle_turn_name
    ON ab_battle_advancement_pool(battle_set_id, turn_index, pool_name);

CREATE UNIQUE INDEX IF NOT EXISTS uq_ab_battle_advancement_decision_pool_turn_job
    ON ab_battle_advancement_decision(battle_advancement_pool_id, turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ab_battle_advancement_decision_pool_kind
    ON ab_battle_advancement_decision(battle_advancement_pool_id, decision_kind);

CREATE INDEX IF NOT EXISTS ix_ab_manual_followup_status
    ON ab_manual_followup(manual_followup_status);

PRAGMA foreign_keys = ON;

COMMIT;
