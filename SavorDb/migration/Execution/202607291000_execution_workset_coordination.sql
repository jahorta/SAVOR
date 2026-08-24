BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

-- A descriptor materializes one job-set population before publishing any
-- immediately claimable worksets from it.
ALTER TABLE exec_job_set
    ADD COLUMN materialization_key TEXT NULL;

ALTER TABLE exec_job_set
    ADD COLUMN materialization_state TEXT NULL
        CHECK(materialization_state IS NULL OR materialization_state IN (
            'MATERIALIZING',
            'POPULATION_SEALED',
            'PUBLISHING_WORKSETS',
            'WORKSET_PUBLICATION_COMPLETE'
        ));

ALTER TABLE exec_job_set
    ADD COLUMN population_sealed_at_utc INTEGER NULL;

ALTER TABLE exec_job_set
    ADD COLUMN workset_publication_completed_at_utc INTEGER NULL;

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_job_set_materialization_key
    ON exec_job_set(materialization_key)
    WHERE materialization_key IS NOT NULL;

-- A workset is an immutable published grouping. Its payload and shared
-- baseline are reconstructed by the program-kind descriptor after claim.
CREATE TABLE IF NOT EXISTS exec_workset (
    workset_id INTEGER PRIMARY KEY,
    job_set_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    workset_key TEXT NOT NULL,
    program_kind INTEGER NOT NULL CHECK(program_kind > 0),
    program_version INTEGER NOT NULL CHECK(program_version > 0),
    compatibility_key TEXT NOT NULL CHECK(length(compatibility_key) > 0),
    module_canonical_id TEXT NOT NULL CHECK(length(module_canonical_id) > 0),
    module_version INTEGER NOT NULL CHECK(module_version > 0),
    module_sha256 TEXT NOT NULL CHECK(
        length(module_sha256) = 64
        AND module_sha256 NOT GLOB '*[^0-9A-Fa-f]*'
    ),
    entrypoint TEXT NOT NULL CHECK(length(entrypoint) > 0),
    verified_dependency_sha256 TEXT NOT NULL
        CHECK(
            length(verified_dependency_sha256) = 64
            AND verified_dependency_sha256 NOT GLOB '*[^0-9A-Fa-f]*'
        ),
    runtime_profile_sha256 TEXT NOT NULL
        CHECK(
            length(runtime_profile_sha256) = 64
            AND runtime_profile_sha256 NOT GLOB '*[^0-9A-Fa-f]*'
        ),
    required_capability_mask INTEGER NOT NULL
        CHECK(required_capability_mask >= 0),
    execution_affinity_key TEXT NULL,
    estimated_payload_bytes INTEGER NOT NULL
        CHECK(estimated_payload_bytes >= 0),
    priority INTEGER NOT NULL,
    item_count INTEGER NOT NULL CHECK(item_count > 0),
    published_at_utc INTEGER NOT NULL,
    FOREIGN KEY(job_set_id) REFERENCES exec_job_set(job_set_id),
    FOREIGN KEY(workflow_step_id)
        REFERENCES exec_workflow_step(workflow_step_id),
    CONSTRAINT uq_exec_workset_job_set_key
        UNIQUE(job_set_id, workset_key)
);

CREATE INDEX IF NOT EXISTS ix_exec_workset_schedule
    ON exec_workset(priority DESC, published_at_utc ASC, workset_id ASC);

CREATE INDEX IF NOT EXISTS ix_exec_workset_compatibility
    ON exec_workset(
        module_canonical_id,
        module_version,
        module_sha256,
        entrypoint,
        required_capability_mask
    );

-- Each row is one durable authority envelope for a runtime dispatch of the
-- logical workset. Only a workset factually resident in a worker is leased.
CREATE TABLE IF NOT EXISTS exec_workset_dispatch_attempt (
    dispatch_attempt_id INTEGER PRIMARY KEY,
    workset_id INTEGER NOT NULL,
    dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
    state TEXT NOT NULL CHECK(state IN ('CLAIMED','ACTIVE','DRAINING','CLOSED')),
    claim_token TEXT NOT NULL,
    lease_expires_at_utc INTEGER NULL,
    claimed_at_utc INTEGER NOT NULL,
    dispatched_at_utc INTEGER NULL,
    draining_at_utc INTEGER NULL,
    closed_at_utc INTEGER NULL,
    close_reason_code TEXT NULL,
    close_reason_text TEXT NULL,
    FOREIGN KEY(workset_id) REFERENCES exec_workset(workset_id),
    CONSTRAINT uq_exec_workset_dispatch_sequence
        UNIQUE(workset_id, dispatch_sequence),
    CONSTRAINT uq_exec_workset_dispatch_token
        UNIQUE(claim_token),
    CHECK(
        (state = 'CLAIMED' AND lease_expires_at_utc IS NULL
            AND dispatched_at_utc IS NULL AND draining_at_utc IS NULL
            AND closed_at_utc IS NULL)
        OR
        (state = 'ACTIVE' AND lease_expires_at_utc IS NOT NULL
            AND dispatched_at_utc IS NOT NULL AND draining_at_utc IS NULL
            AND closed_at_utc IS NULL)
        OR
        (state = 'DRAINING' AND lease_expires_at_utc IS NULL
            AND dispatched_at_utc IS NOT NULL AND draining_at_utc IS NOT NULL
            AND closed_at_utc IS NULL)
        OR
        (state = 'CLOSED' AND lease_expires_at_utc IS NULL
            AND closed_at_utc IS NOT NULL)
    )
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_workset_active_dispatch
    ON exec_workset_dispatch_attempt(workset_id)
    WHERE state IN ('CLAIMED','ACTIVE','DRAINING');

CREATE INDEX IF NOT EXISTS ix_exec_workset_dispatch_lease
    ON exec_workset_dispatch_attempt(state, lease_expires_at_utc);

-- Worker result payloads live in a coordinator-private temporary root. The
-- database stores only validated relative references and cleanup ownership.
CREATE TABLE IF NOT EXISTS exec_temp_blob (
    temp_blob_id INTEGER PRIMARY KEY,
    relative_path TEXT NOT NULL,
    sha256 TEXT NOT NULL CHECK(
        length(sha256) = 64
        AND sha256 NOT GLOB '*[^0-9A-Fa-f]*'
    ),
    size_bytes INTEGER NOT NULL CHECK(size_bytes > 0),
    format TEXT NOT NULL CHECK(length(format) > 0),
    cleanup_state TEXT NOT NULL DEFAULT 'LIVE'
        CHECK(cleanup_state IN ('LIVE','DELETE_PENDING','DELETED')),
    created_at_utc INTEGER NOT NULL,
    delete_pending_at_utc INTEGER NULL,
    cleanup_claim_token TEXT NULL,
    cleanup_lease_expires_at_utc INTEGER NULL,
    cleanup_attempts INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_attempts >= 0),
    cleanup_error TEXT NULL,
    deleted_at_utc INTEGER NULL,
    CONSTRAINT uq_exec_temp_blob_relative_path UNIQUE(relative_path),
    CONSTRAINT uq_exec_temp_blob_sha256_path UNIQUE(sha256, relative_path),
    CHECK(
        relative_path GLOB 'worker_results/*'
        AND length(relative_path) > length('worker_results/')
        AND relative_path NOT LIKE '/%'
        AND relative_path NOT LIKE '\%'
        AND relative_path NOT GLOB '[A-Za-z]:*'
        AND relative_path NOT LIKE '%:%'
        AND relative_path NOT LIKE '%\%'
        AND relative_path NOT LIKE '%//%'
        AND relative_path NOT LIKE '%/./%'
        AND relative_path NOT LIKE '%/.'
        AND relative_path <> '..'
        AND relative_path NOT LIKE '../%'
        AND relative_path NOT LIKE '%/../%'
        AND relative_path NOT LIKE '%/..'
        AND relative_path NOT LIKE '..\%'
        AND relative_path NOT LIKE '%\..\%'
        AND relative_path NOT LIKE '%\..'
    ),
    CHECK(
        (cleanup_claim_token IS NULL
            AND cleanup_lease_expires_at_utc IS NULL)
        OR
        (cleanup_state = 'DELETE_PENDING'
            AND cleanup_claim_token IS NOT NULL
            AND cleanup_lease_expires_at_utc IS NOT NULL)
    ),
    CHECK(
        (cleanup_state = 'LIVE'
            AND delete_pending_at_utc IS NULL
            AND deleted_at_utc IS NULL)
        OR
        (cleanup_state = 'DELETE_PENDING'
            AND delete_pending_at_utc IS NOT NULL
            AND deleted_at_utc IS NULL)
        OR
        (cleanup_state = 'DELETED'
            AND delete_pending_at_utc IS NOT NULL
            AND deleted_at_utc IS NOT NULL)
    )
);

CREATE INDEX IF NOT EXISTS ix_exec_temp_blob_cleanup
    ON exec_temp_blob(cleanup_state, cleanup_lease_expires_at_utc, temp_blob_id);

-- Published membership is stored directly on the job and is assigned exactly
-- once. Dispatch authority is workset-scoped while attempts remain per job.
ALTER TABLE exec_job
    ADD COLUMN workset_id INTEGER NULL
        REFERENCES exec_workset(workset_id);

ALTER TABLE exec_job
    ADD COLUMN workset_item_ordinal INTEGER NULL
        CHECK(workset_item_ordinal IS NULL OR workset_item_ordinal >= 0);

ALTER TABLE exec_job
    ADD COLUMN dispatch_attempt_id INTEGER NULL
        REFERENCES exec_workset_dispatch_attempt(dispatch_attempt_id);

ALTER TABLE exec_job
    ADD COLUMN reserved_attempt_id INTEGER NULL
        CHECK(reserved_attempt_id IS NULL OR reserved_attempt_id > 0);

ALTER TABLE exec_job
    ADD COLUMN execution_finished_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_status TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_fingerprint TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_id TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_error_code TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_error_text TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN worker_terminal_unstarted INTEGER NULL
        CHECK(worker_terminal_unstarted IS NULL OR worker_terminal_unstarted IN (0, 1));

ALTER TABLE exec_job
    ADD COLUMN worker_result_blob_id INTEGER NULL
        REFERENCES exec_temp_blob(temp_blob_id);

ALTER TABLE exec_job
    ADD COLUMN result_processing_state TEXT NULL
        CHECK(result_processing_state IS NULL OR result_processing_state IN (
            'PENDING',
            'PROCESSING',
            'PROCESSED'
        ));

ALTER TABLE exec_job
    ADD COLUMN result_processing_attempts INTEGER NOT NULL DEFAULT 0
        CHECK(result_processing_attempts >= 0);

ALTER TABLE exec_job
    ADD COLUMN result_processing_failures INTEGER NOT NULL DEFAULT 0
        CHECK(result_processing_failures >= 0);

ALTER TABLE exec_job
    ADD COLUMN result_processing_error_code TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN result_processing_error_text TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN result_processing_failed_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN result_processed_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_group_key TEXT NULL
        CHECK(
            cancellation_group_key IS NULL
            OR length(cancellation_group_key) > 0
        );

CREATE INDEX IF NOT EXISTS ix_exec_job_cancellation_group
    ON exec_job(cancellation_group_key, job_id)
    WHERE cancellation_group_key IS NOT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_state TEXT NULL
        CHECK(cancellation_state IS NULL OR cancellation_state IN (
            'REQUESTED',
            'DELIVERED',
            'RESOLVED'
        ));

ALTER TABLE exec_job
    ADD COLUMN cancellation_request_key TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_reason_code TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_reason_text TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_requested_by TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_caused_by_job_id INTEGER NULL
        REFERENCES exec_job(job_id);

ALTER TABLE exec_job
    ADD COLUMN cancellation_requested_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_delivery_attempts INTEGER NOT NULL DEFAULT 0
        CHECK(cancellation_delivery_attempts >= 0);

ALTER TABLE exec_job
    ADD COLUMN cancellation_last_delivery_error_code TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_last_delivery_error_text TEXT NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_last_delivery_failed_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_delivered_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_resolved_at_utc INTEGER NULL;

ALTER TABLE exec_job
    ADD COLUMN cancellation_resolution_code TEXT NULL;

-- Cancellation is requested per job and retains its own durable delivery and
-- resolution history. The fields on exec_job are only a current-state summary.
CREATE TABLE IF NOT EXISTS exec_job_cancellation_request (
    cancellation_request_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    request_key TEXT NOT NULL,
    reason_code TEXT NOT NULL CHECK(length(reason_code) > 0),
    reason_text TEXT NULL,
    requested_by TEXT NOT NULL CHECK(length(requested_by) > 0),
    caused_by_job_id INTEGER NULL,
    requested_at_utc INTEGER NOT NULL,
    state TEXT NOT NULL CHECK(state IN (
        'REQUESTED',
        'DELIVERED',
        'RESOLVED'
    )),
    delivery_attempts INTEGER NOT NULL DEFAULT 0
        CHECK(delivery_attempts >= 0),
    last_delivery_error_code TEXT NULL,
    last_delivery_error_text TEXT NULL,
    last_delivery_failed_at_utc INTEGER NULL,
    delivered_at_utc INTEGER NULL,
    resolved_at_utc INTEGER NULL,
    resolution_code TEXT NULL,
    FOREIGN KEY(job_id) REFERENCES exec_job(job_id),
    FOREIGN KEY(caused_by_job_id) REFERENCES exec_job(job_id),
    CONSTRAINT uq_exec_job_cancel_request_job_key
        UNIQUE(job_id, request_key)
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_job_active_cancellation_request
    ON exec_job_cancellation_request(job_id)
    WHERE state IN ('REQUESTED','DELIVERED');

CREATE INDEX IF NOT EXISTS ix_exec_job_cancellation_request_delivery
    ON exec_job_cancellation_request(
        state,
        requested_at_utc,
        cancellation_request_id
    );

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_job_workset_ordinal
    ON exec_job(workset_id, workset_item_ordinal)
    WHERE workset_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS ix_exec_job_workset_state
    ON exec_job(workset_id, state, workset_item_ordinal);

CREATE INDEX IF NOT EXISTS ix_exec_job_dispatch_attempt
    ON exec_job(dispatch_attempt_id, state, workset_item_ordinal);

CREATE INDEX IF NOT EXISTS ix_exec_job_execution_finished
    ON exec_job(state, result_processing_state, execution_finished_at_utc);

CREATE INDEX IF NOT EXISTS ix_exec_job_cancellation_delivery
    ON exec_job(cancellation_state, cancellation_requested_at_utc);

CREATE TRIGGER IF NOT EXISTS trg_exec_job_workset_membership_insert
BEFORE INSERT ON exec_job
WHEN
    (NEW.workset_id IS NULL) <> (NEW.workset_item_ordinal IS NULL)
    OR (
        NEW.workset_id IS NOT NULL
        AND NOT EXISTS (
            SELECT 1
            FROM exec_workset w
            WHERE w.workset_id = NEW.workset_id
              AND w.job_set_id = NEW.job_set_id
              AND w.program_kind = NEW.program_kind
              AND w.program_version = NEW.program_version
        )
    )
BEGIN
    SELECT RAISE(ABORT, 'invalid exec_job workset membership');
END;

CREATE TRIGGER IF NOT EXISTS trg_exec_job_workset_membership_update
BEFORE UPDATE OF workset_id, workset_item_ordinal ON exec_job
WHEN
    (OLD.workset_id IS NOT NULL AND (
        NEW.workset_id IS NOT OLD.workset_id
        OR NEW.workset_item_ordinal IS NOT OLD.workset_item_ordinal
    ))
    OR
    ((NEW.workset_id IS NULL) <> (NEW.workset_item_ordinal IS NULL))
    OR (
        NEW.workset_id IS NOT NULL
        AND NOT EXISTS (
            SELECT 1
            FROM exec_workset w
            WHERE w.workset_id = NEW.workset_id
              AND w.job_set_id = NEW.job_set_id
              AND w.program_kind = NEW.program_kind
              AND w.program_version = NEW.program_version
        )
    )
BEGIN
    SELECT RAISE(ABORT, 'immutable or invalid exec_job workset membership');
END;

COMMIT;
