BEGIN IMMEDIATE;

CREATE TABLE ab_battle_single_turn_result_v1 (
    battle_single_turn_result_id INTEGER PRIMARY KEY,
    turn_job_id INTEGER NOT NULL UNIQUE,
    exec_job_id INTEGER NOT NULL UNIQUE,
    worker_terminal_sha256 TEXT NOT NULL CHECK(length(worker_terminal_sha256)=64),
    terminal_kind TEXT NOT NULL,
    domain_outcome TEXT NULL,
    error_code TEXT NULL,
    error_text TEXT NULL,
    ending_rng INTEGER NULL,
    vi_start INTEGER NULL,
    vi_end INTEGER NULL,
    pred_passed INTEGER NULL,
    pred_total INTEGER NULL,
    cumulative_fake_attacks INTEGER NULL,
    successor_savestate_id INTEGER NULL,
    battle_context_artifact_id INTEGER NULL,
    predicate_bundle_revision_id INTEGER NULL,
    predicate_bundle_sha256 TEXT NULL,
    predicate_binding_sha256 TEXT NULL,
    predicate_evidence_blob BLOB NULL,
    applied_input_artifact_id INTEGER NULL,
    input_trace_artifact_id INTEGER NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

CREATE INDEX ix_ab_battle_single_turn_result_terminal
    ON ab_battle_single_turn_result_v1(terminal_kind,domain_outcome);

COMMIT;
