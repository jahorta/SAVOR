BEGIN IMMEDIATE;

DROP TABLE IF EXISTS ab_battle_results;
DROP TABLE IF EXISTS ab_battle_recording;
DROP TABLE IF EXISTS ab_battle_completion;

CREATE TABLE ab_battle_completion (
    battle_completion_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    battle_set_id INTEGER NOT NULL,
    wave_id INTEGER NOT NULL,
    selected_turn_job_id INTEGER NOT NULL,
    selected_execution_job_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    completion_savestate_id INTEGER NULL,
    manifest_version INTEGER NULL,
    manifest_blob BLOB NULL,
    manifest_sha256 TEXT NULL,
    manifest_artifact_id INTEGER NULL,
    route_kind TEXT NULL CHECK(route_kind IS NULL OR route_kind IN (
        'OVERWORLD_NAVIGATION','FIELD_NAVIGATION','CUTSCENE','SHIP_BATTLE')),
    transition_filename TEXT NULL,
    worker_terminal_sha256 TEXT NULL,
    error_code TEXT NULL,
    error_text TEXT NULL,
    status TEXT NOT NULL CHECK(status IN ('QUEUED','COMPLETED','FAILED')),
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_completion_workflow_step UNIQUE(workflow_step_id),
    CONSTRAINT uq_ab_battle_completion_selected_job UNIQUE(selected_turn_job_id),
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id),
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id),
    FOREIGN KEY(selected_turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

CREATE TABLE ab_battle_recording (
    battle_recording_id INTEGER PRIMARY KEY,
    battle_completion_id INTEGER NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    source_savestate_id INTEGER NOT NULL,
    source_dtm_artifact_id INTEGER NOT NULL,
    source_itinerary_artifact_id INTEGER NOT NULL,
    source_binding_version INTEGER NOT NULL,
    source_binding_blob BLOB NOT NULL,
    source_binding_sha256 TEXT NOT NULL,
    replay_plan_version INTEGER NOT NULL,
    replay_plan_blob BLOB NOT NULL,
    replay_plan_sha256 TEXT NOT NULL,
    outcome TEXT NULL CHECK(outcome IS NULL OR outcome IN ('RECORDED','REPLAY_MISMATCH')),
    recorded_dtm_artifact_id INTEGER NULL,
    recorded_itinerary_artifact_id INTEGER NULL,
    paired_checkpoint_savestate_id INTEGER NULL,
    timing_anchor_version INTEGER NULL,
    timing_anchor_blob BLOB NULL,
    tas_movie_tree_id INTEGER NULL,
    validation_request_id INTEGER NULL,
    sterilization_request_id INTEGER NULL,
    worker_terminal_sha256 TEXT NULL,
    error_code TEXT NULL,
    error_text TEXT NULL,
    status TEXT NOT NULL CHECK(status IN ('QUEUED','COMPLETED','REPLAY_MISMATCH','FAILED')),
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_recording_workflow_step UNIQUE(workflow_step_id),
    CONSTRAINT uq_ab_battle_recording_completion UNIQUE(battle_completion_id),
    FOREIGN KEY(battle_completion_id) REFERENCES ab_battle_completion(battle_completion_id)
);

CREATE INDEX ix_ab_battle_completion_exec_job
    ON ab_battle_completion(exec_job_id);
CREATE INDEX ix_ab_battle_completion_lineage
    ON ab_battle_completion(battle_set_id,wave_id,selected_turn_job_id);
CREATE INDEX ix_ab_battle_completion_output_state
    ON ab_battle_completion(completion_savestate_id);
CREATE INDEX ix_ab_battle_recording_exec_job
    ON ab_battle_recording(exec_job_id);
CREATE INDEX ix_ab_battle_recording_tree
    ON ab_battle_recording(tas_movie_tree_id);
CREATE INDEX ix_ab_battle_recording_validation
    ON ab_battle_recording(validation_request_id,sterilization_request_id);

COMMIT;
