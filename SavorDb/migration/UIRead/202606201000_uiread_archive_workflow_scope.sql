BEGIN IMMEDIATE;

ALTER TABLE ui_archive_catalog
    ADD COLUMN source_scope_kind TEXT NOT NULL DEFAULT 'job_set';

ALTER TABLE ui_archive_catalog
    ADD COLUMN source_workflow_count INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ui_archive_catalog
    ADD COLUMN selection_summary TEXT NULL;

CREATE INDEX IF NOT EXISTS ix_ui_archive_catalog_scope
    ON ui_archive_catalog(source_scope_kind, created_at_utc DESC);

COMMIT;
