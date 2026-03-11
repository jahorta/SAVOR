-- 0032.sql
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (32, strftime('%s','now'));

DROP VIEW IF EXISTS v_job_set_progress;
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
  SUM(CASE WHEN j.state IN ('SUCCEEDED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END) AS succeeded,
  SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END) AS failed,
  SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END) AS canceled,
  SUM(CASE WHEN j.state='SUPERSEDED' THEN 1 ELSE 0 END) AS superseded,
  SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END) AS winner,
  SUM(CASE WHEN j.state='SUCCEEDED_DUPLICATE' THEN 1 ELSE 0 END) AS duplicate
FROM job_sets js
LEFT JOIN jobs j ON j.job_set_id = js.job_set_id
GROUP BY js.job_set_id;

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
  SUM(CASE WHEN j.state IN ('SUCCEEDED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END) AS succeeded,
  SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END) AS failed,
  SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END) AS canceled,
  SUM(CASE WHEN j.state='SUPERSEDED' THEN 1 ELSE 0 END) AS superseded,
  SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END) AS winner,
  SUM(CASE WHEN j.state='SUCCEEDED_DUPLICATE' THEN 1 ELSE 0 END) AS duplicate
FROM r
LEFT JOIN jobs j ON j.job_set_id = r.job_set_id
GROUP BY r.root_id;

PRAGMA foreign_keys=ON;
COMMIT;
