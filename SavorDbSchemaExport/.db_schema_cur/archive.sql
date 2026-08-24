CREATE TABLE migration_history (
    context TEXT NOT NULL,
    migration_name TEXT NOT NULL,
    applied_at_utc INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (context, migration_name)
);
CREATE TABLE migration_schema_version (
    context TEXT PRIMARY KEY,
    version INTEGER NOT NULL
);
CREATE TABLE ar_archive_package (
    archive_package_id INTEGER PRIMARY KEY,
    source_context TEXT NOT NULL,
    source_job_set_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    schema_version INTEGER NOT NULL,
    event_catalog_version INTEGER NOT NULL,
    time_range_start_utc INTEGER NOT NULL,
    time_range_end_utc INTEGER NOT NULL,
    manifest_path TEXT NOT NULL,
    checksum_status TEXT NOT NULL
);
CREATE TABLE ar_archive_item (
    archive_item_id INTEGER PRIMARY KEY,
    archive_package_id INTEGER NOT NULL,
    item_kind TEXT NOT NULL,
    item_count INTEGER NOT NULL,
    blob_path TEXT NULL,
    checksum TEXT NULL,
    FOREIGN KEY(archive_package_id) REFERENCES ar_archive_package(archive_package_id)
);
CREATE TABLE ar_rehydrate_request (
    rehydrate_request_id INTEGER PRIMARY KEY,
    archive_package_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    error_text TEXT NULL,
    target_namespace TEXT NOT NULL,
    FOREIGN KEY(archive_package_id) REFERENCES ar_archive_package(archive_package_id)
);
CREATE TABLE ar_rehydrate_map (
    rehydrate_map_id INTEGER PRIMARY KEY,
    rehydrate_request_id INTEGER NOT NULL,
    entity_kind TEXT NOT NULL,
    old_id TEXT NOT NULL,
    new_id TEXT NOT NULL,
    FOREIGN KEY(rehydrate_request_id) REFERENCES ar_rehydrate_request(rehydrate_request_id)
);
CREATE INDEX ix_ar_archive_item_package
    ON ar_archive_item(archive_package_id, item_kind);
CREATE INDEX ix_ar_rehydrate_request_status
    ON ar_rehydrate_request(status, requested_at_utc DESC);
CREATE INDEX ix_ar_rehydrate_map_lookup
    ON ar_rehydrate_map(rehydrate_request_id, entity_kind, old_id);
CREATE TABLE ar_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NULL,
    causation_id TEXT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    CONSTRAINT uq_ar_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_ar_outbox_unpublished
    ON ar_outbox_message(published_at_utc, outbox_id);
CREATE TABLE ar_archive_item_kind_catalog (
    item_kind TEXT PRIMARY KEY,
    include_by_default INTEGER NOT NULL CHECK(include_by_default IN (0, 1)),
    notes TEXT NULL
);
