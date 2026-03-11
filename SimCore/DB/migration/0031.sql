-- 0031.sql
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (31, strftime('%s','now'));

-- Rebuilding jobs requires dropping dependent views first; otherwise schema
-- validation can fail while jobs is temporarily absent.
DROP VIEW IF EXISTS v_job_queue_ready;
DROP VIEW IF EXISTS v_job_set_progress;
DROP VIEW IF EXISTS v_winners_progress;
DROP VIEW IF EXISTS v_job_set_progress_h;
DROP VIEW IF EXISTS v_winners_progress_h;

CREATE TABLE jobs__new (
    job_id           INTEGER PRIMARY KEY,
    job_set_id       INTEGER NOT NULL REFERENCES job_sets(job_set_id),
    program_kind     INTEGER NOT NULL REFERENCES program_kinds(kind_id),
    program_version  INTEGER NOT NULL,
    program_ref_id   INTEGER NOT NULL,
    fingerprint      TEXT    NOT NULL UNIQUE,
    priority         INTEGER NOT NULL DEFAULT 0,
    state            TEXT    NOT NULL CHECK(state IN
                       ('QUEUED','INTERRUPTED','CLAIMED','RUNNING','SUCCEEDED','FAILED',
                        'CANCELED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE')),
    attempts         INTEGER NOT NULL DEFAULT 0,
    max_attempts     INTEGER NOT NULL DEFAULT 5,
    claimed_by_token TEXT    NULL,
    lease_expires_at INTEGER NULL,
    queued_at        INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    vm_kv            TEXT    NULL,
    savestate_id     INTEGER REFERENCES savestate(id)
);

INSERT INTO jobs__new(
    job_id, job_set_id, program_kind, program_version, program_ref_id,
    fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at, queued_at, vm_kv, savestate_id
)
SELECT
    job_id, job_set_id, program_kind, program_version, program_ref_id,
    fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at, queued_at, vm_kv, savestate_id
FROM jobs;

DROP TABLE jobs;
ALTER TABLE jobs__new RENAME TO jobs;

CREATE INDEX IF NOT EXISTS ix_jobs_queue   ON jobs(state, program_kind, queued_at DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_lease   ON jobs(lease_expires_at);
CREATE INDEX IF NOT EXISTS ix_jobs_token   ON jobs(claimed_by_token);
CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset ON jobs(job_set_id);
CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset_state_queued ON jobs(job_set_id, state, queued_at DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_queued_id_desc ON jobs(queued_at DESC, job_id DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_savestate ON jobs(savestate_id);
CREATE INDEX IF NOT EXISTS ix_jobs_state_savestate_queued ON jobs(state, savestate_id, queued_at DESC);

CREATE VIEW v_job_queue_ready AS
SELECT * FROM jobs WHERE state IN ('QUEUED','INTERRUPTED');

CREATE VIEW v_job_set_progress AS
SELECT
  js.job_set_id,
  COUNT(j.job_id)                             AS total,
  SUM(CASE WHEN j.state IN ('SUCCEEDED','FAILED','CANCELED','SUPERSEDED',
                            'SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END) AS terminal,
  ROUND(100.0 * SUM(CASE WHEN j.state IN ('SUCCEEDED','FAILED','CANCELED','SUPERSEDED',
                                          'SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END)
              / NULLIF(COUNT(j.job_id),0), 1) AS pct_complete,
  SUM(CASE WHEN j.state='QUEUED' THEN 1 ELSE 0 END) AS queued,
  SUM(CASE WHEN j.state='CLAIMED' THEN 1 ELSE 0 END) AS claimed,
  SUM(CASE WHEN j.state='RUNNING' THEN 1 ELSE 0 END) AS running,
  SUM(CASE WHEN j.state='SUCCEEDED' THEN 1 ELSE 0 END) AS succeeded,
  SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END) AS failed,
  SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END) AS canceled,
  SUM(CASE WHEN j.state='SUPERSEDED' THEN 1 ELSE 0 END) AS superseded,
  SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END) AS winner,
  SUM(CASE WHEN j.state='SUCCEEDED_DUPLICATE' THEN 1 ELSE 0 END) AS duplicate
FROM job_sets js
LEFT JOIN jobs j ON j.job_set_id = js.job_set_id
GROUP BY js.job_set_id;

CREATE VIEW v_winners_progress AS
SELECT
  js.job_set_id,
  (SELECT COUNT(1) FROM seed_probe_winners w WHERE w.job_set_id = js.job_set_id) AS winners_found,
  js.expected_total AS expected_total
FROM job_sets js;

CREATE VIEW v_job_set_progress_h AS
WITH RECURSIVE r(root_id, job_set_id) AS (
  SELECT js.job_set_id, js.job_set_id FROM job_sets js
  UNION ALL
  SELECT r.root_id, c.job_set_id
  FROM job_sets c
  JOIN r ON c.parent_job_set_id = r.job_set_id
)
SELECT
  r.root_id AS job_set_id,
  COUNT(j.job_id) AS total,
  SUM(CASE WHEN j.state IN ('SUCCEEDED','FAILED','CANCELED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END) AS terminal,
  ROUND(100.0 * SUM(CASE WHEN j.state IN ('SUCCEEDED','FAILED','CANCELED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END) / NULLIF(COUNT(j.job_id),0), 1) AS pct_complete,
  SUM(CASE WHEN j.state='QUEUED' THEN 1 ELSE 0 END) AS queued,
  SUM(CASE WHEN j.state='CLAIMED' THEN 1 ELSE 0 END) AS claimed,
  SUM(CASE WHEN j.state='RUNNING' THEN 1 ELSE 0 END) AS running,
  SUM(CASE WHEN j.state='SUCCEEDED' THEN 1 ELSE 0 END) AS succeeded,
  SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END) AS failed,
  SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END) AS canceled,
  SUM(CASE WHEN j.state='SUPERSEDED' THEN 1 ELSE 0 END) AS superseded,
  SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END) AS winner,
  SUM(CASE WHEN j.state='SUCCEEDED_DUPLICATE' THEN 1 ELSE 0 END) AS duplicate
FROM r
LEFT JOIN jobs j ON j.job_set_id = r.job_set_id
GROUP BY r.root_id;

CREATE VIEW v_winners_progress_h AS
SELECT
  js.job_set_id,
  (SELECT COUNT(1) FROM seed_probe_winners w WHERE w.job_set_id = js.job_set_id) AS winners_found,
  js.expected_total AS expected_total
FROM job_sets js;

PRAGMA foreign_keys=ON;
COMMIT;
