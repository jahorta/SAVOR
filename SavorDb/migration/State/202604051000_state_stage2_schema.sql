BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS state_artifact (
    artifact_id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    compression_kind INTEGER NOT NULL,
    filename TEXT NOT NULL,
    file_ext TEXT NOT NULL,
    artifact_kind TEXT NOT NULL CHECK(artifact_kind IN ('DTM', 'DTMINI', 'SAV', 'LOG', 'OTHER')),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_state_artifact_sha256 UNIQUE (sha256)
);

CREATE TABLE IF NOT EXISTS state_savestate (
    savestate_id INTEGER PRIMARY KEY,
    artifact_id INTEGER NOT NULL,
    savestate_type TEXT NOT NULL,
    note TEXT NULL,
    is_complete INTEGER NOT NULL CHECK(is_complete IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(artifact_id) REFERENCES state_artifact(artifact_id)
);

CREATE TABLE IF NOT EXISTS state_savestate_derivation (
    derivation_id INTEGER PRIMARY KEY,
    from_savestate_id INTEGER NOT NULL,
    to_savestate_id INTEGER NOT NULL,
    method_kind TEXT NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(from_savestate_id) REFERENCES state_savestate(savestate_id),
    FOREIGN KEY(to_savestate_id) REFERENCES state_savestate(savestate_id)
);

CREATE TABLE IF NOT EXISTS state_tas_movie_variant (
    tas_variant_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    base_dtm_artifact_id INTEGER NOT NULL,
    dtmini_artifact_id INTEGER NULL,
    mutation_mode TEXT NOT NULL CHECK(mutation_mode IN ('NONE', 'RTC_OVERRIDE', 'INSERT_NEUTRAL_FRAME')),
    rtc_value INTEGER NULL,
    bookmark_name TEXT NULL,
    insert_frame_count INTEGER NULL,
    parent_tas_variant_id INTEGER NULL,
    produced_savestate_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(base_dtm_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(dtmini_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(parent_tas_variant_id) REFERENCES state_tas_movie_variant(tas_variant_id),
    FOREIGN KEY(produced_savestate_id) REFERENCES state_savestate(savestate_id),
    CONSTRAINT uq_state_tas_movie_variant_name UNIQUE (name)
);

CREATE TABLE IF NOT EXISTS state_outbox_message (
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
    CONSTRAINT uq_state_outbox_event_id UNIQUE (event_id)
);

CREATE INDEX IF NOT EXISTS ix_state_savestate_derivation_from
    ON state_savestate_derivation(from_savestate_id);

CREATE INDEX IF NOT EXISTS ix_state_savestate_derivation_to
    ON state_savestate_derivation(to_savestate_id);

CREATE INDEX IF NOT EXISTS ix_state_outbox_unpublished
    ON state_outbox_message(published_at_utc, outbox_id);

COMMIT;
