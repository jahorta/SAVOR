ALTER TABLE ab_battle_set
ADD COLUMN continue_automatic_exploration_after_victory INTEGER NOT NULL DEFAULT 0
CHECK(continue_automatic_exploration_after_victory IN (0,1));

ALTER TABLE ab_battle_start
ADD COLUMN continue_automatic_exploration_after_victory INTEGER NOT NULL DEFAULT 0
CHECK(continue_automatic_exploration_after_victory IN (0,1));

BEGIN IMMEDIATE;
PRAGMA foreign_keys=OFF;

ALTER TABLE ab_battle_advancement_decision
RENAME TO ab_battle_advancement_decision_old;

CREATE TABLE ab_battle_advancement_decision (
    battle_advancement_decision_id INTEGER PRIMARY KEY,
    battle_advancement_pool_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    decision_kind TEXT NOT NULL CHECK(decision_kind IN (
        'SELECTED','NOT_SELECTED','REJECTED',
        'SELECTED_FOR_COMPLETION','DUPLICATE_ENDING_RNG',
        'RECOMMENDED','NOT_RECOMMENDED')),
    decision_reason TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_advancement_pool_id)
        REFERENCES ab_battle_advancement_pool(battle_advancement_pool_id),
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

INSERT INTO ab_battle_advancement_decision(
    battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,
    decision_kind,decision_reason,created_at_utc)
SELECT battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,
       decision_kind,decision_reason,created_at_utc
FROM ab_battle_advancement_decision_old;

DROP TABLE ab_battle_advancement_decision_old;

CREATE UNIQUE INDEX uq_ab_battle_advancement_decision_pool_turn_job
ON ab_battle_advancement_decision(battle_advancement_pool_id,turn_job_id);

CREATE INDEX ix_ab_battle_advancement_decision_pool_kind
ON ab_battle_advancement_decision(battle_advancement_pool_id,decision_kind);

PRAGMA foreign_keys=ON;
COMMIT;
