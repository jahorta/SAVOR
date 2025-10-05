-- 0021.sql  Keyset-friendly global indexes for UI lists

BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (21, strftime('%s','now'));

-- Jobs: global most-recent scan regardless of state/program_kind
CREATE INDEX IF NOT EXISTS ix_jobs_queued_id_desc
  ON jobs(queued_at DESC, job_id DESC);

-- Job events: global tail (system log) and efficient paging
CREATE INDEX IF NOT EXISTS ix_job_events_ts_id_desc
  ON job_events(ts DESC, event_id DESC);

-- Job sets: global most-recent across all kinds
CREATE INDEX IF NOT EXISTS ix_job_sets_created_id_desc
  ON job_sets(created_at DESC, job_set_id DESC);

COMMIT;
