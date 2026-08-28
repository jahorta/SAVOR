BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = OFF;

DROP INDEX IF EXISTS ix_ui_battle_followup_status;
DROP INDEX IF EXISTS ix_ui_battle_manual_followup_status;

ALTER TABLE ui_battle_wave ADD COLUMN parent_turn_job_id INTEGER NULL;

ALTER TABLE ui_battle_turn_job ADD COLUMN has_desired_outcome INTEGER NOT NULL DEFAULT 0 CHECK(has_desired_outcome IN (0, 1));
ALTER TABLE ui_battle_turn_job ADD COLUMN selected_for_advancement INTEGER NOT NULL DEFAULT 0 CHECK(selected_for_advancement IN (0, 1));
ALTER TABLE ui_battle_turn_job ADD COLUMN advancement_decision_kind TEXT NULL;
ALTER TABLE ui_battle_turn_job ADD COLUMN advancement_rank INTEGER NOT NULL DEFAULT 0 CHECK(advancement_rank IN (0, 1, 2));

CREATE TABLE IF NOT EXISTS ui_battle_advancement_decision (
    battle_advancement_decision_id INTEGER PRIMARY KEY,
    battle_advancement_pool_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    decision_kind TEXT NOT NULL CHECK(decision_kind IN (
        'SELECTED', 'NOT_SELECTED', 'REJECTED',
        'SELECTED_FOR_COMPLETION', 'DUPLICATE_ENDING_RNG',
        'RECOMMENDED', 'NOT_RECOMMENDED')),
    decision_reason TEXT NULL,
    created_at_utc INTEGER NOT NULL
);

ALTER TABLE ui_battle_followup RENAME TO ui_battle_manual_followup_old;

CREATE TABLE ui_battle_manual_followup (
    turn_job_id INTEGER PRIMARY KEY,
    manual_followup_status TEXT NOT NULL,
    recorded_dtm_artifact_id INTEGER NULL,
    note TEXT NULL,
    updated_at_utc INTEGER NOT NULL
);

INSERT INTO ui_battle_manual_followup(
    turn_job_id,
    manual_followup_status,
    recorded_dtm_artifact_id,
    note,
    updated_at_utc)
SELECT
    turn_job_id,
    manual_followup_status,
    recorded_dtm_artifact_id,
    note,
    updated_at_utc
FROM ui_battle_manual_followup_old;

DROP TABLE ui_battle_manual_followup_old;

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_advancement
    ON ui_battle_turn_job(advancement_rank, has_desired_outcome, selected_for_advancement);

CREATE INDEX IF NOT EXISTS ix_ui_battle_advancement_decision_turn_job
    ON ui_battle_advancement_decision(turn_job_id, decision_kind);

CREATE INDEX IF NOT EXISTS ix_ui_battle_manual_followup_status
    ON ui_battle_manual_followup(manual_followup_status);

PRAGMA foreign_keys = ON;

COMMIT;
