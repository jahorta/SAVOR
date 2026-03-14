-- 0007.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (7, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS program_kinds (
    kind_id           INTEGER PRIMARY KEY,         -- matches Wire.h PK_*
    name              TEXT    NOT NULL UNIQUE,
    base_priority     INTEGER NOT NULL DEFAULT 0,
    learned_spawn_ms  INTEGER NOT NULL DEFAULT 0,
    created_at        INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS job_sets (
    job_set_id            INTEGER PRIMARY KEY,
    purpose               TEXT,
    program_kind          INTEGER NOT NULL REFERENCES program_kinds(kind_id),
    created_by            TEXT,
    created_at            INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    domain_ref_kind       TEXT    NULL,
    domain_ref_id         INTEGER NULL,
    meta_text             TEXT    NULL,           -- INI or freeform text
    expected_total        INTEGER NULL
);

CREATE INDEX IF NOT EXISTS ix_job_sets_kind ON job_sets(program_kind, created_at DESC);

COMMIT;
