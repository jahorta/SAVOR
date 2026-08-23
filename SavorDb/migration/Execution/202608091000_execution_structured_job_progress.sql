BEGIN IMMEDIATE;

ALTER TABLE exec_workset
    ADD COLUMN capture_binding_payload BLOB NULL;
ALTER TABLE exec_workset
    ADD COLUMN capture_binding_sha256 TEXT NULL;
ALTER TABLE exec_workset
    ADD COLUMN progress_plan_payload BLOB NULL;
ALTER TABLE exec_workset
    ADD COLUMN progress_plan_sha256 TEXT NULL;

CREATE TABLE exec_job_progress (
    job_id INTEGER NOT NULL,
    attempt_id INTEGER NOT NULL CHECK(attempt_id > 0),
    ordinal INTEGER NOT NULL CHECK(ordinal > 0),
    dispatch_attempt_id INTEGER NOT NULL,
    workset_item_ordinal INTEGER NOT NULL,
    workset_id INTEGER NOT NULL,
    item_id INTEGER NOT NULL,
    invocation_id INTEGER NOT NULL,
    library_id TEXT NOT NULL,
    library_revision INTEGER NOT NULL CHECK(library_revision > 0),
    progress_point_id TEXT NOT NULL,
    has_routed_provenance INTEGER NOT NULL CHECK(has_routed_provenance IN (0, 1)),
    routed_sequence INTEGER NULL,
    sample_snapshot_id INTEGER NULL,
    trigger_epoch INTEGER NULL,
    schema_id TEXT NOT NULL,
    schema_revision INTEGER NOT NULL CHECK(schema_revision > 0),
    schema_sha256 TEXT NOT NULL,
    typed_payload BLOB NOT NULL,
    display_text TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    PRIMARY KEY(job_id, attempt_id, ordinal),
    FOREIGN KEY(job_id) REFERENCES exec_job(job_id),
    CHECK(
        (has_routed_provenance = 0
            AND routed_sequence IS NULL
            AND sample_snapshot_id IS NULL
            AND trigger_epoch IS NULL)
        OR
        (has_routed_provenance = 1
            AND routed_sequence IS NOT NULL
            AND sample_snapshot_id IS NOT NULL
            AND trigger_epoch IS NOT NULL)
    )
);

CREATE INDEX ix_exec_job_progress_recent
    ON exec_job_progress(job_id, attempt_id DESC, ordinal DESC);

COMMIT;
