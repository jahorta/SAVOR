-- 0035: add explicit parent_job_id link for derived jobs
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (35, strftime('%s','now'));

ALTER TABLE jobs ADD COLUMN parent_job_id INTEGER NULL REFERENCES jobs(job_id);
CREATE INDEX IF NOT EXISTS ix_jobs_parent_job_id ON jobs(parent_job_id);

PRAGMA foreign_keys=ON;
COMMIT;
