-- Migration 0006: db_event table
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (6, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS db_event (
    id               INTEGER PRIMARY KEY,
    kind             TEXT    NOT NULL,
    created_mono_ns  INTEGER NOT NULL,
    created_utc      TEXT    NOT NULL,
    coord_boot_id    TEXT    NOT NULL,
    payload          TEXT
);

CREATE INDEX IF NOT EXISTS ix_event_mono  ON db_event(created_mono_ns);
CREATE INDEX IF NOT EXISTS ix_event_kind  ON db_event(kind, created_mono_ns DESC);

COMMIT;
