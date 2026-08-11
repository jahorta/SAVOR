BEGIN IMMEDIATE;

ALTER TABLE ui_job_summary
    ADD COLUMN last_progress_attempt_id INTEGER NULL;
ALTER TABLE ui_job_summary
    ADD COLUMN last_progress_ordinal INTEGER NULL;
ALTER TABLE ui_job_summary
    ADD COLUMN last_progress_text TEXT NULL;
ALTER TABLE ui_job_summary
    ADD COLUMN last_progress_at_utc INTEGER NULL;

CREATE TABLE ui_job_progress (
    job_id INTEGER NOT NULL,
    attempt_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    workset_id INTEGER NOT NULL,
    item_id INTEGER NOT NULL,
    invocation_id INTEGER NOT NULL,
    library_id TEXT NOT NULL,
    library_revision INTEGER NOT NULL,
    progress_point_id TEXT NOT NULL,
    routed_sequence INTEGER NULL,
    sample_snapshot_id INTEGER NULL,
    trigger_epoch INTEGER NULL,
    schema_id TEXT NOT NULL,
    schema_revision INTEGER NOT NULL,
    schema_sha256 TEXT NOT NULL,
    typed_payload BLOB NOT NULL,
    display_text TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    PRIMARY KEY(job_id, attempt_id, ordinal)
);

CREATE INDEX ix_ui_job_progress_recent
    ON ui_job_progress(job_id, attempt_id DESC, ordinal DESC);

COMMIT;
