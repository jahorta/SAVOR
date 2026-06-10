BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS ui_archive_rehydrate_request (
    rehydrate_request_id INTEGER PRIMARY KEY,
    archive_package_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    target_namespace TEXT NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    error_text TEXT NULL
);

CREATE INDEX IF NOT EXISTS ix_ui_archive_rehydrate_request_recent
    ON ui_archive_rehydrate_request(requested_at_utc DESC, rehydrate_request_id DESC);

CREATE INDEX IF NOT EXISTS ix_ui_archive_rehydrate_request_status_recent
    ON ui_archive_rehydrate_request(status, requested_at_utc DESC, rehydrate_request_id DESC);

COMMIT;
