BEGIN IMMEDIATE;

CREATE TABLE ab_historical_failed_battle_single_turn_results (
    historical_failed_battle_single_turn_result_id INTEGER PRIMARY KEY,
    superseded_battle_single_turn_result_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    exec_job_id INTEGER NOT NULL,
    prior_worker_terminal_sha256 TEXT NOT NULL CHECK(length(prior_worker_terminal_sha256)=64),
    replacement_worker_terminal_sha256 TEXT NOT NULL CHECK(length(replacement_worker_terminal_sha256)=64),
    prior_terminal_kind TEXT NOT NULL CHECK(prior_terminal_kind='FAILED'),
    prior_domain_outcome TEXT NULL,
    prior_error_code TEXT NULL,
    prior_error_text TEXT NULL,
    prior_recorded_at_utc INTEGER NOT NULL,
    superseded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(superseded_battle_single_turn_result_id)
        REFERENCES ab_battle_single_turn_result_v1(battle_single_turn_result_id),
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

CREATE INDEX ix_ab_historical_failed_battle_single_turn_results_exec_job
    ON ab_historical_failed_battle_single_turn_results(exec_job_id, superseded_at_utc);
CREATE INDEX ix_ab_historical_failed_battle_single_turn_results_turn_job
    ON ab_historical_failed_battle_single_turn_results(turn_job_id, superseded_at_utc);
CREATE INDEX ix_ab_historical_failed_battle_single_turn_results_predecessor
    ON ab_historical_failed_battle_single_turn_results(superseded_battle_single_turn_result_id);

COMMIT;
