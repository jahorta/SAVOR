BEGIN IMMEDIATE;

ALTER TABLE ui_archive_catalog
    ADD COLUMN archive_name TEXT NOT NULL DEFAULT '';

ALTER TABLE ui_archive_catalog
    ADD COLUMN archive_notes TEXT NULL;

CREATE INDEX IF NOT EXISTS ix_ui_archive_catalog_name
    ON ui_archive_catalog(archive_name, created_at_utc DESC);

COMMIT;
