-- Migration 0014: battle_contexts + triggers tables

PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (14, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS battle_contexts (
  context_id   INTEGER PRIMARY KEY,
  job_set_id   INTEGER NOT NULL,
  job_id       INTEGER NOT NULL,
  artifact_id  INTEGER NOT NULL,
  created_at   INTEGER DEFAULT (strftime('%s','now')),
  savestate_id INTEGER NOT NULL,
  bc_version   INTEGER NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS ux_battle_context_job ON battle_contexts(job_id);
CREATE INDEX IF NOT EXISTS ix_battle_context_jobset ON battle_contexts(job_set_id);

CREATE TABLE IF NOT EXISTS triggers (
  trigger_id   INTEGER PRIMARY KEY,
  scope        TEXT NOT NULL,     -- 'job' | 'job_set'
  scope_id     INTEGER NOT NULL,
  condition    TEXT NOT NULL,
  action_kind  TEXT NOT NULL,
  action_args  TEXT NOT NULL,
  created_at   INTEGER DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS ix_triggers_scope ON triggers(scope, scope_id);

COMMIT;
PRAGMA foreign_keys=ON;
