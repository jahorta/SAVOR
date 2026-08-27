BEGIN IMMEDIATE;

CREATE TABLE tmv_input_epoch_annotation_request (
    annotation_request_id INTEGER PRIMARY KEY,
    materialization_key TEXT NOT NULL UNIQUE,
    workflow_instance_id INTEGER NOT NULL CHECK(workflow_instance_id>0),
    workflow_step_id INTEGER NOT NULL UNIQUE CHECK(workflow_step_id>0),
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    full_phase_program_kind INTEGER NOT NULL CHECK(full_phase_program_kind IN (13,100)),
    full_phase_program_version INTEGER NOT NULL CHECK(full_phase_program_version>0),
    full_phase_canonical_id TEXT NOT NULL,
    full_phase_contract_revision INTEGER NOT NULL CHECK(full_phase_contract_revision>0),
    full_phase_sha256 TEXT NOT NULL,
    module_canonical_id TEXT NOT NULL,
    module_revision INTEGER NOT NULL CHECK(module_revision>0),
    module_sha256 TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE tmv_input_epoch_annotation_attempt (
    annotation_attempt_id INTEGER PRIMARY KEY,
    producer_kind TEXT NOT NULL CHECK(producer_kind IN ('ANNOTATE','REVISE')),
    annotation_request_id INTEGER NULL,
    rewrite_request_id INTEGER NULL,
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    source_job_id INTEGER NOT NULL CHECK(source_job_id>0),
    worker_terminal_sha256 TEXT NOT NULL,
    succeeded INTEGER NOT NULL CHECK(succeeded IN (0,1)),
    schedule_artifact_id INTEGER NULL CHECK(schedule_artifact_id IS NULL OR schedule_artifact_id>0),
    schedule_sha256 TEXT NULL,
    source_poll_count INTEGER NOT NULL CHECK(source_poll_count>=0),
    epoch_count INTEGER NOT NULL CHECK(epoch_count>=0),
    final_cursor INTEGER NOT NULL CHECK(final_cursor>=0),
    divergence_epoch INTEGER NULL CHECK(divergence_epoch IS NULL OR divergence_epoch>=0),
    divergence_cursor INTEGER NULL CHECK(divergence_cursor IS NULL OR divergence_cursor>=0),
    failure_code TEXT NOT NULL,
    failure_text TEXT NOT NULL,
    worker_id TEXT NOT NULL,
    worker_process_generation INTEGER NOT NULL CHECK(worker_process_generation>=0),
    workset_epoch INTEGER NOT NULL CHECK(workset_epoch>0),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(annotation_request_id)
        REFERENCES tmv_input_epoch_annotation_request(annotation_request_id)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY(rewrite_request_id)
        REFERENCES tmv_input_epoch_rewrite_request(rewrite_request_id)
        DEFERRABLE INITIALLY DEFERRED,
    UNIQUE(source_job_id,worker_terminal_sha256),
    CHECK((producer_kind='ANNOTATE' AND annotation_request_id IS NOT NULL AND rewrite_request_id IS NULL)
       OR (producer_kind='REVISE' AND annotation_request_id IS NULL AND rewrite_request_id IS NOT NULL)),
    CHECK((succeeded=1 AND schedule_artifact_id IS NOT NULL AND schedule_sha256 IS NOT NULL
               AND failure_code='' AND failure_text='')
       OR (succeeded=0 AND schedule_artifact_id IS NULL AND schedule_sha256 IS NULL
               AND failure_code<>''))
);

CREATE INDEX ix_tmv_input_epoch_annotation_attempt_request
    ON tmv_input_epoch_annotation_attempt(annotation_request_id,annotation_attempt_id);

CREATE TABLE tmv_input_epoch_rewrite_request (
    rewrite_request_id INTEGER PRIMARY KEY,
    materialization_key TEXT NOT NULL UNIQUE,
    workflow_instance_id INTEGER NOT NULL CHECK(workflow_instance_id>0),
    workflow_step_id INTEGER NOT NULL UNIQUE CHECK(workflow_step_id>0),
    annotation_attempt_id INTEGER NOT NULL CHECK(annotation_attempt_id>0),
    root_establishment_attempt_id INTEGER NOT NULL CHECK(root_establishment_attempt_id>0),
    source_dtm_artifact_id INTEGER NOT NULL CHECK(source_dtm_artifact_id>0),
    source_dtm_sha256 TEXT NOT NULL,
    schedule_artifact_id INTEGER NOT NULL CHECK(schedule_artifact_id>0),
    schedule_sha256 TEXT NOT NULL,
    insert_before_epoch INTEGER NOT NULL CHECK(insert_before_epoch>=0),
    neutral_epoch_count INTEGER NOT NULL CHECK(neutral_epoch_count>=0),
    placement_profile TEXT NOT NULL,
    full_phase_program_kind INTEGER NOT NULL CHECK(full_phase_program_kind=14),
    full_phase_program_version INTEGER NOT NULL CHECK(full_phase_program_version>0),
    full_phase_canonical_id TEXT NOT NULL,
    full_phase_contract_revision INTEGER NOT NULL CHECK(full_phase_contract_revision>0),
    full_phase_sha256 TEXT NOT NULL,
    module_canonical_id TEXT NOT NULL,
    module_revision INTEGER NOT NULL CHECK(module_revision>0),
    module_sha256 TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(annotation_attempt_id)
        REFERENCES tmv_input_epoch_annotation_attempt(annotation_attempt_id)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY(root_establishment_attempt_id)
        REFERENCES tmv_root_establishment_attempt(root_establishment_attempt_id)
        DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE tmv_input_epoch_rewrite_attempt (
    rewrite_attempt_id INTEGER PRIMARY KEY,
    rewrite_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL CHECK(source_job_id>0),
    worker_terminal_sha256 TEXT NOT NULL,
    succeeded INTEGER NOT NULL CHECK(succeeded IN (0,1)),
    rewritten_dtm_artifact_id INTEGER NULL CHECK(rewritten_dtm_artifact_id IS NULL OR rewritten_dtm_artifact_id>0),
    rewritten_dtm_sha256 TEXT NULL,
    endpoint_savestate_id INTEGER NULL CHECK(endpoint_savestate_id IS NULL OR endpoint_savestate_id>0),
    produced_annotation_attempt_id INTEGER NULL CHECK(produced_annotation_attempt_id IS NULL OR produced_annotation_attempt_id>0),
    produced_root_establishment_attempt_id INTEGER NULL CHECK(produced_root_establishment_attempt_id IS NULL OR produced_root_establishment_attempt_id>0),
    source_epoch_count INTEGER NOT NULL CHECK(source_epoch_count>=0),
    rewritten_epoch_count INTEGER NOT NULL CHECK(rewritten_epoch_count>=0),
    final_movie_input_count INTEGER NOT NULL CHECK(final_movie_input_count>=0),
    divergence_epoch INTEGER NULL CHECK(divergence_epoch IS NULL OR divergence_epoch>=0),
    divergence_cursor INTEGER NULL CHECK(divergence_cursor IS NULL OR divergence_cursor>=0),
    failure_code TEXT NOT NULL,
    failure_text TEXT NOT NULL,
    worker_id TEXT NOT NULL,
    worker_process_generation INTEGER NOT NULL CHECK(worker_process_generation>=0),
    workset_epoch INTEGER NOT NULL CHECK(workset_epoch>0),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(rewrite_request_id)
        REFERENCES tmv_input_epoch_rewrite_request(rewrite_request_id),
    FOREIGN KEY(produced_annotation_attempt_id)
        REFERENCES tmv_input_epoch_annotation_attempt(annotation_attempt_id)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY(produced_root_establishment_attempt_id)
        REFERENCES tmv_root_establishment_attempt(root_establishment_attempt_id)
        DEFERRABLE INITIALLY DEFERRED,
    UNIQUE(source_job_id,worker_terminal_sha256),
    CHECK((succeeded=1 AND rewritten_dtm_artifact_id IS NOT NULL
               AND rewritten_dtm_sha256 IS NOT NULL AND endpoint_savestate_id IS NOT NULL
               AND failure_code='' AND failure_text='')
       OR (succeeded=0 AND rewritten_dtm_artifact_id IS NULL
               AND rewritten_dtm_sha256 IS NULL AND endpoint_savestate_id IS NULL
               AND failure_code<>''))
);

CREATE INDEX ix_tmv_input_epoch_rewrite_attempt_request
    ON tmv_input_epoch_rewrite_attempt(rewrite_request_id,rewrite_attempt_id);

COMMIT;
