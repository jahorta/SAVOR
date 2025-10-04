BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (20, strftime('%s','now'));

CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset
  ON jobs(job_set_id);

CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset_state_queued
  ON jobs(job_set_id, state, queued_at DESC);

COMMIT;
