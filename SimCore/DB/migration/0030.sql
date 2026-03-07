-- 0030_jobs_savestate_id.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (30, strftime('%s','now'));

ALTER TABLE jobs ADD COLUMN savestate_id INTEGER REFERENCES savestate(id);

-- Helpful indexes
CREATE INDEX IF NOT EXISTS ix_jobs_savestate ON jobs(savestate_id);
CREATE INDEX IF NOT EXISTS ix_jobs_state_savestate_queued
  ON jobs(state, savestate_id, queued_at DESC);

COMMIT;
