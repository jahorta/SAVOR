BEGIN IMMEDIATE;

CREATE TABLE tmv_validation_request (
    validation_request_id INTEGER PRIMARY KEY,
    materialization_key TEXT NOT NULL UNIQUE,
    workflow_instance_id INTEGER NOT NULL CHECK(workflow_instance_id > 0),
    workflow_step_id INTEGER NOT NULL UNIQUE CHECK(workflow_step_id > 0),
    step_kind TEXT NOT NULL CHECK(step_kind IN
        ('tasmovie.establish_root_cursor','tasmovie.validate_root','tasmovie.validate_tree')),
    operation TEXT NOT NULL CHECK(operation IN ('ESTABLISH_ROOT_CURSOR','VALIDATE')),
    source_kind TEXT NOT NULL CHECK(source_kind IN ('DTM_ARTIFACT','ROOT_ESTABLISHMENT','TREE')),
    source_ref_id INTEGER NOT NULL CHECK(source_ref_id > 0),
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id > 0),
    source_dtm_sha256 TEXT NOT NULL,
    rtc_value INTEGER NULL CHECK(rtc_value IS NULL OR rtc_value >= 0),
    effective_dtm_sha256 TEXT NOT NULL,
    itinerary_artifact_id INTEGER NULL CHECK(itinerary_artifact_id IS NULL OR itinerary_artifact_id > 0),
    itinerary_sha256 TEXT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL CHECK(required_final_breakpoint_pc BETWEEN 0 AND 4294967295),
    capture_root_checkpoint INTEGER NOT NULL CHECK(capture_root_checkpoint IN (0,1)),
    full_phase_program_kind INTEGER NOT NULL,
    full_phase_program_version INTEGER NOT NULL,
    full_phase_canonical_id TEXT NOT NULL,
    full_phase_contract_revision INTEGER NOT NULL,
    full_phase_sha256 TEXT NOT NULL,
    module_canonical_id TEXT NOT NULL,
    module_revision INTEGER NOT NULL,
    module_sha256 TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CHECK(
        (operation='ESTABLISH_ROOT_CURSOR' AND source_kind='DTM_ARTIFACT'
            AND rtc_value IS NULL AND itinerary_artifact_id IS NULL
            AND itinerary_sha256 IS NULL AND capture_root_checkpoint=0)
        OR
        (operation='VALIDATE' AND source_kind IN ('ROOT_ESTABLISHMENT','TREE')
            AND itinerary_artifact_id IS NOT NULL AND itinerary_sha256 IS NOT NULL
            AND ((source_kind='ROOT_ESTABLISHMENT' AND rtc_value IS NOT NULL)
                OR (source_kind='TREE' AND rtc_value IS NULL AND capture_root_checkpoint=0)))
    )
);

CREATE TABLE tmv_validation_attempt (
    validation_attempt_id INTEGER PRIMARY KEY,
    validation_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL CHECK(source_job_id > 0),
    worker_terminal_sha256 TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK(outcome IN ('ROOT_CURSOR_ESTABLISHED','VALID','INVALID')),
    failure_reason TEXT NULL CHECK(failure_reason IS NULL OR failure_reason IN
        ('MOVIE_DESYNCHRONIZED','EXPECTED_TERMINAL_NOT_REACHED','UNKNOWN')),
    expected_pc INTEGER NULL CHECK(expected_pc IS NULL OR expected_pc BETWEEN 0 AND 4294967295),
    expected_input_count INTEGER NULL CHECK(expected_input_count IS NULL OR expected_input_count >= 0),
    actual_pc INTEGER NOT NULL CHECK(actual_pc BETWEEN 0 AND 4294967295),
    actual_input_count INTEGER NOT NULL CHECK(actual_input_count >= 0),
    last_verified_itinerary_index INTEGER NULL CHECK(last_verified_itinerary_index IS NULL OR last_verified_itinerary_index >= 0),
    last_known_good_savestate_id INTEGER NULL CHECK(last_known_good_savestate_id IS NULL OR last_known_good_savestate_id > 0),
    candidate_itinerary_artifact_id INTEGER NULL CHECK(candidate_itinerary_artifact_id IS NULL OR candidate_itinerary_artifact_id > 0),
    candidate_itinerary_sha256 TEXT NULL,
    produced_tas_movie_root_id INTEGER NULL CHECK(produced_tas_movie_root_id IS NULL OR produced_tas_movie_root_id > 0),
    worker_id TEXT NOT NULL,
    worker_process_generation INTEGER NOT NULL CHECK(worker_process_generation >= 0),
    workset_epoch INTEGER NOT NULL CHECK(workset_epoch >= 0),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(validation_request_id) REFERENCES tmv_validation_request(validation_request_id),
    UNIQUE(source_job_id,worker_terminal_sha256),
    CHECK(
        (outcome='ROOT_CURSOR_ESTABLISHED' AND failure_reason IS NULL
            AND candidate_itinerary_artifact_id IS NOT NULL AND candidate_itinerary_sha256 IS NOT NULL)
        OR (outcome='VALID' AND failure_reason IS NULL
            AND candidate_itinerary_artifact_id IS NULL AND candidate_itinerary_sha256 IS NULL)
        OR (outcome='INVALID' AND failure_reason IS NOT NULL
            AND candidate_itinerary_artifact_id IS NULL AND candidate_itinerary_sha256 IS NULL)
    )
);

CREATE TABLE tmv_dtm_validation_status (
    effective_dtm_sha256 TEXT PRIMARY KEY,
    status TEXT NOT NULL CHECK(status IN ('VALID','QUARANTINED')),
    validation_attempt_id INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL,
    FOREIGN KEY(validation_attempt_id) REFERENCES tmv_validation_attempt(validation_attempt_id)
);

CREATE INDEX ix_tmv_validation_attempt_request
    ON tmv_validation_attempt(validation_request_id,validation_attempt_id);
CREATE INDEX ix_tmv_validation_attempt_job
    ON tmv_validation_attempt(source_job_id,validation_attempt_id);

CREATE TABLE tmv_root_establishment_attempt (
    root_establishment_attempt_id INTEGER PRIMARY KEY,
    producer_kind TEXT NOT NULL CHECK(producer_kind IN ('ESTABLISH','REVISE')),
    validation_attempt_id INTEGER NULL,
    rewrite_request_id INTEGER NULL,
    parent_root_establishment_attempt_id INTEGER NULL,
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    itinerary_artifact_id INTEGER NOT NULL CHECK(itinerary_artifact_id>0),
    itinerary_sha256 TEXT NOT NULL,
    root_pc INTEGER NOT NULL CHECK(root_pc>0),
    movie_input_cursor INTEGER NOT NULL CHECK(movie_input_cursor>=0),
    source_job_id INTEGER NOT NULL CHECK(source_job_id>0),
    worker_terminal_sha256 TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(validation_attempt_id) REFERENCES tmv_validation_attempt(validation_attempt_id)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY(rewrite_request_id) REFERENCES tmv_input_epoch_rewrite_request(rewrite_request_id)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY(parent_root_establishment_attempt_id)
        REFERENCES tmv_root_establishment_attempt(root_establishment_attempt_id)
        DEFERRABLE INITIALLY DEFERRED,
    UNIQUE(source_job_id,worker_terminal_sha256),
    CHECK((producer_kind='ESTABLISH' AND validation_attempt_id IS NOT NULL AND rewrite_request_id IS NULL)
       OR (producer_kind='REVISE' AND validation_attempt_id IS NULL AND rewrite_request_id IS NOT NULL))
);

CREATE INDEX ix_tmv_root_establishment_source
    ON tmv_root_establishment_attempt(source_dtm_artifact_id,root_establishment_attempt_id);

COMMIT;
