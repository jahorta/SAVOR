BEGIN IMMEDIATE;

DROP TABLE IF EXISTS ab_battle_replay;

CREATE TABLE ab_battle_replay (
    battle_replay_id INTEGER PRIMARY KEY,
    battle_completion_id INTEGER NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    source_savestate_id INTEGER NOT NULL,
    source_dtm_artifact_id INTEGER NULL,
    source_itinerary_artifact_id INTEGER NULL,
    source_binding_version INTEGER NOT NULL,
    source_binding_blob BLOB NOT NULL,
    source_binding_sha256 TEXT NOT NULL,
    replay_plan_version INTEGER NOT NULL,
    replay_plan_blob BLOB NOT NULL,
    replay_plan_sha256 TEXT NOT NULL,
    outcome TEXT NULL CHECK(outcome IS NULL OR outcome IN ('MATCHED','REPLAY_MISMATCH')),
    mismatch_turn INTEGER NOT NULL DEFAULT 0,
    expected_rng INTEGER NOT NULL DEFAULT 0,
    observed_rng INTEGER NOT NULL DEFAULT 0,
    observed_completion_blob BLOB NULL,
    observed_completion_sha256 TEXT NULL,
    observed_transition_blob BLOB NULL,
    observed_transition_sha256 TEXT NULL,
    worker_terminal_sha256 TEXT NULL,
    error_code TEXT NULL,
    error_text TEXT NULL,
    status TEXT NOT NULL CHECK(status IN ('QUEUED','MATCHED','REPLAY_MISMATCH','FAILED')),
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_replay_workflow_step UNIQUE(workflow_step_id),
    CONSTRAINT uq_ab_battle_replay_completion UNIQUE(battle_completion_id),
    FOREIGN KEY(battle_completion_id) REFERENCES ab_battle_completion(battle_completion_id)
);

CREATE INDEX ix_ab_battle_replay_exec_job
    ON ab_battle_replay(exec_job_id);

COMMIT;
