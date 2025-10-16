-- 0028.sql: parent->child job_sets + hierarchical rollups
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (28, strftime('%s','now'));

-- Parentage + optional delta key hint (INTEGER for simplicity)
ALTER TABLE job_sets ADD COLUMN parent_job_set_id INTEGER NULL REFERENCES job_sets(job_set_id) ON DELETE CASCADE;
CREATE INDEX IF NOT EXISTS ix_job_sets_parent ON job_sets(parent_job_set_id);

-- Hierarchical job_set progress: per root, aggregate over all descendants (including root)
DROP VIEW IF EXISTS v_job_set_progress_h;
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

-- Hierarchical winners progress: keep same shape; winners are keyed to the root (parent) job_set
DROP VIEW IF EXISTS v_winners_progress_h;
CREATE VIEW v_winners_progress_h AS
SELECT
  js.job_set_id,
  (SELECT COUNT(1) FROM seed_probe_winners w WHERE w.job_set_id = js.job_set_id) AS winners_found,
  js.expected_total AS expected_total
FROM job_sets js;

PRAGMA foreign_keys=ON;
COMMIT;