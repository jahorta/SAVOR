ALTER TABLE ab_turn_job ADD COLUMN source_savestate_id INTEGER NULL;
ALTER TABLE ab_turn_job ADD COLUMN seed_candidate_id INTEGER NULL;
ALTER TABLE ab_turn_job ADD COLUMN authored_plan_id INTEGER NULL;
ALTER TABLE ab_turn_job ADD COLUMN authored_turn_index INTEGER NULL;
ALTER TABLE ab_turn_job ADD COLUMN resolved_turn_commands_blob TEXT NULL;
ALTER TABLE ab_turn_job ADD COLUMN resolved_turn_variant_key TEXT NULL;
ALTER TABLE ab_turn_job ADD COLUMN input_trace_artifact_id INTEGER NULL;

UPDATE ab_turn_job
SET authored_plan_id = COALESCE(authored_plan_id, plan_id),
    input_trace_artifact_id = COALESCE(input_trace_artifact_id, applied_input_artifact_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_job_source_savestate
    ON ab_turn_job(source_savestate_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_job_seed_candidate
    ON ab_turn_job(seed_candidate_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_job_input_trace
    ON ab_turn_job(input_trace_artifact_id);
