-- 0036: visual replay entries queue/log
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (36, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS visual_replay_entries (
    visual_replay_id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id INTEGER NOT NULL REFERENCES jobs(job_id),
    requested_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    state TEXT NOT NULL CHECK(state IN ('QUEUED','RUNNING','SUCCEEDED','FAILED','CANCELED')),
    worker_id INTEGER NULL,
    started_at INTEGER NULL,
    ended_at INTEGER NULL,
    error_text TEXT NULL
);

CREATE INDEX IF NOT EXISTS ix_visual_replay_state_requested
    ON visual_replay_entries(state, requested_at ASC);
CREATE INDEX IF NOT EXISTS ix_visual_replay_job
    ON visual_replay_entries(job_id);

PRAGMA foreign_keys=ON;
COMMIT;
