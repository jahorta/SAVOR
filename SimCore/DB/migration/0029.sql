-- 0029.sql: remove battle_run_groups and group_id from explorer_run
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (29, strftime('%s','now'));

-- Drop dependent view before table rebuild
DROP VIEW IF EXISTS v_lineage;

-- Drop unique index that referenced group_id
DROP INDEX IF EXISTS ux_explorer_run_group_settings_plan_delta;

-- Rebuild explorer_run without group_id
CREATE TABLE explorer_run__new (
  id                         INTEGER PRIMARY KEY,
  probe_id                   INTEGER NOT NULL REFERENCES seed_probe(id) ON DELETE CASCADE,
  settings_id                INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  plan_id                    INTEGER     REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  delta_seed_id              INTEGER     REFERENCES seed_delta(id) ON DELETE CASCADE,
  status                     TEXT    NOT NULL DEFAULT 'planned',
  progress_log               TEXT    NOT NULL DEFAULT '',
  complete                   INTEGER NOT NULL DEFAULT 0,
  results_ini                TEXT,
  progress_log_artifact_id   INTEGER
);

INSERT INTO explorer_run__new(
  id, probe_id, settings_id, plan_id, delta_seed_id, status, progress_log, complete, results_ini, progress_log_artifact_id
)
SELECT
  id, probe_id, settings_id, plan_id, delta_seed_id, status, progress_log, complete, results_ini, progress_log_artifact_id
FROM explorer_run;

DROP TABLE explorer_run;
ALTER TABLE explorer_run__new RENAME TO explorer_run;

-- Drop group table and its index
DROP INDEX IF EXISTS ix_brg_name_desc;
DROP TABLE IF EXISTS battle_run_groups;

-- Recreate v_lineage (latest definition uses delta_seed_id)
CREATE VIEW IF NOT EXISTS v_lineage AS
SELECT
  r.id AS run_id,
  r.probe_id,
  r.delta_seed_id,
  r.settings_id,
  p.savestate_id
FROM explorer_run r
JOIN seed_probe p ON r.probe_id = p.id;

PRAGMA foreign_keys=ON;
COMMIT;
