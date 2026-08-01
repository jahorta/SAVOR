BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS ab_battle_completion (
    battle_completion_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    entry_savestate_id INTEGER NOT NULL,
    completion_savestate_id INTEGER NULL,
    entry_rng_seed INTEGER NULL,
    completion_rng_seed INTEGER NULL,
    manifest_version INTEGER NULL,
    manifest_blob BLOB NULL,
    manifest_artifact_id INTEGER NULL,
    input_trace_artifact_id INTEGER NULL,
    mismatch_count INTEGER NOT NULL DEFAULT 0 CHECK(mismatch_count >= 0),
    invariant_failure_count INTEGER NOT NULL DEFAULT 0 CHECK(invariant_failure_count >= 0),
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_completion_workflow_step UNIQUE (workflow_step_id)
);

CREATE TABLE IF NOT EXISTS ab_battle_results (
    battle_results_id INTEGER PRIMARY KEY,
    battle_completion_id INTEGER NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    selected_seed_ref_kind TEXT NOT NULL CHECK(selected_seed_ref_kind IN (
        'analysisseedprobe.confirmed_result'
    )),
    selected_seed_ref_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    final_savestate_id INTEGER NULL,
    selected_seed_value INTEGER NOT NULL CHECK(selected_seed_value BETWEEN 0 AND 4294967295),
    entry_rng_seed INTEGER NULL CHECK(entry_rng_seed IS NULL OR entry_rng_seed BETWEEN 0 AND 4294967295),
    final_rng_seed INTEGER NULL CHECK(final_rng_seed IS NULL OR final_rng_seed BETWEEN 0 AND 4294967295),
    rng_effect_kind TEXT NOT NULL CHECK(rng_effect_kind IN ('PRESERVE', 'ADVANCE_FIXED', 'VARIABLE')),
    fixed_draw_count INTEGER NULL CHECK(fixed_draw_count IS NULL OR fixed_draw_count >= 0),
    result_artifact_id INTEGER NULL,
    input_trace_artifact_id INTEGER NULL,
    mismatch_count INTEGER NOT NULL DEFAULT 0 CHECK(mismatch_count >= 0),
    invariant_failure_count INTEGER NOT NULL DEFAULT 0 CHECK(invariant_failure_count >= 0),
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    FOREIGN KEY(battle_completion_id) REFERENCES ab_battle_completion(battle_completion_id),
    FOREIGN KEY(selected_seed_ref_id) REFERENCES sp_probe_result(probe_result_id),
    CONSTRAINT uq_ab_battle_results_workflow_step UNIQUE (workflow_step_id)
);

CREATE INDEX IF NOT EXISTS ix_ab_battle_completion_workflow
    ON ab_battle_completion(workflow_instance_id, workflow_step_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_completion_exec_job
    ON ab_battle_completion(exec_job_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_completion_output_state
    ON ab_battle_completion(completion_savestate_id);

CREATE INDEX IF NOT EXISTS ix_ab_battle_results_workflow
    ON ab_battle_results(workflow_instance_id, workflow_step_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_results_exec_job
    ON ab_battle_results(exec_job_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_results_completion
    ON ab_battle_results(battle_completion_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_results_seed_ref
    ON ab_battle_results(selected_seed_ref_kind, selected_seed_ref_id);
CREATE INDEX IF NOT EXISTS ix_ab_battle_results_final_state
    ON ab_battle_results(final_savestate_id);

COMMIT;
