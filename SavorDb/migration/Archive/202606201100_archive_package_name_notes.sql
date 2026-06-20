BEGIN IMMEDIATE;

ALTER TABLE ar_archive_package
    ADD COLUMN archive_name TEXT NOT NULL DEFAULT '';

ALTER TABLE ar_archive_package
    ADD COLUMN archive_notes TEXT NULL;

CREATE INDEX IF NOT EXISTS ix_ar_archive_package_name
    ON ar_archive_package(archive_name, created_at_utc DESC);

COMMIT;
