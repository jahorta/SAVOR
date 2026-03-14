-- 0010.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (10, strftime('%s','now'));

CREATE VIEW IF NOT EXISTS v_job_queue_ready AS
SELECT * FROM jobs WHERE state='QUEUED';

CREATE VIEW IF NOT EXISTS v_job_set_progress AS
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

CREATE VIEW IF NOT EXISTS v_winners_progress AS
SELECT
  js.job_set_id,
  js.expected_total_deltas AS expected_total_deltas,
  SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END) AS winners_found,
  ROUND(100.0 * SUM(CASE WHEN j.state='SUCCEEDED_WINNER' THEN 1 ELSE 0 END)
              / NULLIF(js.expected_total_deltas,0), 1) AS pct_winners
FROM job_sets js
LEFT JOIN jobs j ON j.job_set_id = js.job_set_id
GROUP BY js.job_set_id;

COMMIT;
