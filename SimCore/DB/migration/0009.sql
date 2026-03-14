-- 0009.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (9, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS job_events (
    event_id   INTEGER PRIMARY KEY,
    job_id     INTEGER NOT NULL REFERENCES jobs(job_id),
    ts         INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    event_kind TEXT    NOT NULL,    -- PROGRESS, STARTED, SUCCEEDED, FAILED, etc.
    payload    TEXT    NULL         -- single append-log line (no JSON)
);

CREATE INDEX IF NOT EXISTS ix_job_events_job_ts ON job_events(job_id, ts DESC);

COMMIT;
