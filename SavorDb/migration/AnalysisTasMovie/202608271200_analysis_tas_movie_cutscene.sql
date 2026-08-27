BEGIN IMMEDIATE;

CREATE TABLE tmv_cutscene_request (
    cutscene_request_id INTEGER PRIMARY KEY,
    materialization_key TEXT NOT NULL UNIQUE,
    workflow_instance_id INTEGER NOT NULL CHECK(workflow_instance_id>0),
    workflow_step_id INTEGER NOT NULL UNIQUE CHECK(workflow_step_id>0),
    source_validation_attempt_id INTEGER NOT NULL CHECK(source_validation_attempt_id>0),
    source_tree_id INTEGER NOT NULL CHECK(source_tree_id>0),
    source_savestate_id INTEGER NOT NULL CHECK(source_savestate_id>0),
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    source_itinerary_artifact_id INTEGER NOT NULL CHECK(source_itinerary_artifact_id>0),
    source_itinerary_sha256 TEXT NOT NULL,
    source_movie_input_cursor INTEGER NOT NULL CHECK(source_movie_input_cursor>0),
    full_phase_program_kind INTEGER NOT NULL CHECK(full_phase_program_kind=15),
    full_phase_program_version INTEGER NOT NULL CHECK(full_phase_program_version>0),
    full_phase_canonical_id TEXT NOT NULL,
    full_phase_contract_revision INTEGER NOT NULL CHECK(full_phase_contract_revision>0),
    full_phase_sha256 TEXT NOT NULL,
    module_canonical_id TEXT NOT NULL,
    module_revision INTEGER NOT NULL CHECK(module_revision>0),
    module_sha256 TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE tmv_cutscene_attempt (
    cutscene_attempt_id INTEGER PRIMARY KEY,
    cutscene_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL CHECK(source_job_id>0),
    worker_terminal_sha256 TEXT NOT NULL,
    succeeded INTEGER NOT NULL CHECK(succeeded IN (0,1)),
    endpoint_kind TEXT NOT NULL,
    endpoint_pc INTEGER NOT NULL CHECK(endpoint_pc>=0),
    checkpoint_movie_input_cursor INTEGER NOT NULL CHECK(checkpoint_movie_input_cursor>=0),
    final_movie_input_cursor INTEGER NOT NULL CHECK(final_movie_input_cursor>=0),
    output_dtm_artifact_id INTEGER NULL CHECK(output_dtm_artifact_id IS NULL OR output_dtm_artifact_id>0),
    output_dtm_sha256 TEXT NULL,
    output_savestate_id INTEGER NULL CHECK(output_savestate_id IS NULL OR output_savestate_id>0),
    output_itinerary_artifact_id INTEGER NULL CHECK(output_itinerary_artifact_id IS NULL OR output_itinerary_artifact_id>0),
    output_tree_id INTEGER NULL CHECK(output_tree_id IS NULL OR output_tree_id>0),
    failure_code TEXT NOT NULL,
    failure_text TEXT NOT NULL,
    worker_id TEXT NOT NULL,
    worker_process_generation INTEGER NOT NULL CHECK(worker_process_generation>=0),
    workset_epoch INTEGER NOT NULL CHECK(workset_epoch>0),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(cutscene_request_id) REFERENCES tmv_cutscene_request(cutscene_request_id),
    UNIQUE(source_job_id,worker_terminal_sha256),
    CHECK((succeeded=1 AND endpoint_kind<>'' AND endpoint_pc>0
               AND checkpoint_movie_input_cursor>0 AND final_movie_input_cursor>checkpoint_movie_input_cursor
               AND output_dtm_artifact_id IS NOT NULL AND output_dtm_sha256 IS NOT NULL
               AND output_savestate_id IS NOT NULL AND output_itinerary_artifact_id IS NOT NULL
               AND output_tree_id IS NOT NULL AND failure_code='' AND failure_text='')
       OR (succeeded=0 AND output_dtm_artifact_id IS NULL AND output_dtm_sha256 IS NULL
               AND output_savestate_id IS NULL AND output_itinerary_artifact_id IS NULL
               AND output_tree_id IS NULL AND failure_code<>''))
);

CREATE INDEX ix_tmv_cutscene_attempt_request
    ON tmv_cutscene_attempt(cutscene_request_id,cutscene_attempt_id);

COMMIT;
