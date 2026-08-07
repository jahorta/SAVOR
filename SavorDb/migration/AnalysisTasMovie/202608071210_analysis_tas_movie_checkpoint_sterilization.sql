BEGIN IMMEDIATE;

CREATE TABLE tmv_checkpoint_sterilization_request (
    sterilization_request_id INTEGER PRIMARY KEY,
    materialization_key TEXT NOT NULL UNIQUE,
    workflow_instance_id INTEGER NOT NULL CHECK(workflow_instance_id>0),
    workflow_step_id INTEGER NOT NULL UNIQUE CHECK(workflow_step_id>0),
    source_savestate_id INTEGER NOT NULL CHECK(source_savestate_id>0),
    source_savestate_artifact_id INTEGER NOT NULL CHECK(source_savestate_artifact_id>0),
    source_savestate_sha256 TEXT NOT NULL,
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    reused_savestate_id INTEGER NULL CHECK(reused_savestate_id IS NULL OR reused_savestate_id>0),
    full_phase_program_kind INTEGER NOT NULL,
    full_phase_program_version INTEGER NOT NULL,
    full_phase_canonical_id TEXT NOT NULL,
    full_phase_contract_revision INTEGER NOT NULL,
    full_phase_sha256 TEXT NOT NULL,
    module_canonical_id TEXT NOT NULL,
    module_revision INTEGER NOT NULL,
    module_sha256 TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE tmv_checkpoint_sterilization_attempt (
    sterilization_attempt_id INTEGER PRIMARY KEY,
    sterilization_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL CHECK(source_job_id>0),
    worker_terminal_sha256 TEXT NOT NULL,
    candidate_savestate_sha256 TEXT NOT NULL,
    produced_savestate_id INTEGER NOT NULL CHECK(produced_savestate_id>0),
    worker_id TEXT NOT NULL,
    worker_process_generation INTEGER NOT NULL CHECK(worker_process_generation>=0),
    workset_epoch INTEGER NOT NULL CHECK(workset_epoch>0),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(sterilization_request_id)
        REFERENCES tmv_checkpoint_sterilization_request(sterilization_request_id),
    UNIQUE(source_job_id,worker_terminal_sha256)
);

CREATE INDEX ix_tmv_checkpoint_sterilization_attempt_request
    ON tmv_checkpoint_sterilization_attempt(
        sterilization_request_id,sterilization_attempt_id);

COMMIT;
