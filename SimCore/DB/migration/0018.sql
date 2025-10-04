-- 0018_explorer_run_keys.sql
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (18, strftime('%s','now'));

-- Rebuild explorer_run to add (group_id already exists from 0017), plan_id, delta_seed_id
CREATE TABLE explorer_run__new (
  id            INTEGER PRIMARY KEY,
  probe_id      INTEGER NOT NULL REFERENCES seed_probe(id) ON DELETE CASCADE,
  settings_id   INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  plan_id       INTEGER REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  delta_seed_id INTEGER REFERENCES seed_delta(id) ON DELETE CASCADE,
  group_id      INTEGER REFERENCES battle_run_groups(group_id) ON DELETE CASCADE,
  status        TEXT    NOT NULL DEFAULT 'planned',
  progress_log  TEXT    NOT NULL DEFAULT '',
  complete      INTEGER NOT NULL DEFAULT 0,
  results_ini   TEXT,
  progress_log_artifact_id INTEGER
);

INSERT INTO explorer_run__new(id, probe_id, settings_id, plan_id, delta_seed_id, group_id, status, progress_log, complete, results_ini, progress_log_artifact_id)
SELECT
  id,
  probe_id,
  settings_id,
  NULL,          -- plan_id (new)
  NULL,          -- delta_seed_id (new)
  group_id,      -- from 0017
  status,
  progress_log,
  complete,
  results_ini,
  progress_log_artifact_id
FROM explorer_run;

DROP TABLE explorer_run;
ALTER TABLE explorer_run__new RENAME TO explorer_run;

-- One-run-per-(group,settings,plan,delta) uniqueness within groups
CREATE UNIQUE INDEX IF NOT EXISTS ux_explorer_run_group_settings_plan_delta
ON explorer_run(group_id, settings_id, plan_id, delta_seed_id)
WHERE group_id IS NOT NULL AND plan_id IS NOT NULL AND delta_seed_id IS NOT NULL;

PRAGMA foreign_keys=ON;
COMMIT;
