-- 0039.sql: shrink explorer_run to minimal run-record schema
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (39, strftime('%s','now'));

DROP VIEW IF EXISTS v_lineage;
DROP TABLE IF EXISTS explorer_run;

CREATE TABLE explorer_run (
  id              INTEGER PRIMARY KEY,
  root_job_set_id INTEGER NOT NULL REFERENCES job_sets(job_set_id) ON DELETE CASCADE,
  settings_id     INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  plan_id         INTEGER NOT NULL REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  delta_seed_id   INTEGER NOT NULL REFERENCES seed_delta(id) ON DELETE CASCADE,
  has_victory     INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS ix_explorer_run_root_job_set
  ON explorer_run(root_job_set_id);

CREATE UNIQUE INDEX IF NOT EXISTS ux_explorer_run_root_settings_plan_delta
  ON explorer_run(root_job_set_id, settings_id, plan_id, delta_seed_id);

PRAGMA foreign_keys=ON;
COMMIT;
