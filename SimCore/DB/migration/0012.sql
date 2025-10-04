-- 0012.sql
-- seed_probe_winners (Milestone 3: online dedupe keyed by realized result_delta)

-- Creates table to record first-success winners per (job_set_id, result_delta).
-- Grid runs NEVER claim winners; only unique-phase runs insert here.
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (12, strftime('%s','now'));

CREATE TABLE seed_probe_winners (
  job_set_id       INTEGER NOT NULL REFERENCES job_sets(job_set_id) ON DELETE CASCADE,
  result_delta     INTEGER NOT NULL,                 -- realized signed delta (rng_seed - neutral_seed)
  winner_job_id    INTEGER NOT NULL REFERENCES jobs(job_id) ON DELETE CASCADE,

  expected_delta   INTEGER,                          -- optional hint from blueprint (for match vs unexpected)
  expected_tag     TEXT,                             -- optional human tag for the expected delta
  stage            TEXT,                             -- e.g. 'UNIQUE' (reserved for analytics)
  frame_hex        TEXT,                             -- input frame hex (audit)
  metrics_json     TEXT,                             -- small JSON blob with rng_seed, timings, etc.

  created_at       INTEGER NOT NULL DEFAULT (CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER)),

  PRIMARY KEY(job_set_id, result_delta)
) WITHOUT ROWID;

COMMIT;
