-- 0005.sql

BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (5, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS tas_movie (
    id INTEGER PRIMARY KEY,
    base_file_id INTEGER NOT NULL REFERENCES object_ref(id),
    new_rtc INTEGER NULL,
    status TEXT NOT NULL DEFAULT 'planned' CHECK(status IN ('planned','running','done')),
    progress_log TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    started_at INTEGER NULL,
    completed_at INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    priority INTEGER NOT NULL DEFAULT 0,
    output_savestate_id INTEGER NULL REFERENCES savestate(id)
);

CREATE INDEX IF NOT EXISTS ix_tas_movie_active
    ON tas_movie(status)
    WHERE status IN ('planned','running');

CREATE INDEX IF NOT EXISTS ix_tas_movie_priority
    ON tas_movie(status, priority DESC, id);

ALTER TABLE savestate
    ADD COLUMN tas_movie_id INTEGER NULL REFERENCES tas_movie(id);

CREATE INDEX IF NOT EXISTS ix_savestate_tas_movie_id
    ON savestate(tas_movie_id);

COMMIT;