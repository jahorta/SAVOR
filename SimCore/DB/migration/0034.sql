-- 0034_debug_sessions.sql
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS debug_sessions (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  job_id            INTEGER NOT NULL REFERENCES jobs(job_id) ON DELETE CASCADE,
  state             TEXT NOT NULL,
  created_at        INTEGER NOT NULL DEFAULT (strftime('%s','now')),
  updated_at        INTEGER NOT NULL DEFAULT (strftime('%s','now')),
  started_by        TEXT,
  worker_id         INTEGER,
  slot_id           INTEGER,
  session_token     TEXT,
  vm_endpoint       TEXT,
  dolphin_endpoint  TEXT,
  lock_acquired_at  INTEGER,
  lock_released_at  INTEGER,
  failure_code      TEXT,
  failure_detail    TEXT
);

CREATE INDEX IF NOT EXISTS ix_debug_sessions_job ON debug_sessions(job_id);
CREATE INDEX IF NOT EXISTS ix_debug_sessions_state ON debug_sessions(state, updated_at DESC);
