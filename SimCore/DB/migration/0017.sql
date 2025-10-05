-- 0017.sql battlerungroup planlink
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (17, strftime('%s','now'));

DROP VIEW IF EXISTS v_winners_progress;

-- Battle run groups
CREATE TABLE IF NOT EXISTS battle_run_groups (
  group_id     INTEGER PRIMARY KEY,
  settings_id  INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  seed_probe_id INTEGER NOT NULL REFERENCES seed_probe(id) ON DELETE CASCADE,
  name         TEXT,
  description  TEXT,
  predicate_vector_fingerprint TEXT,
  plan_vector_fingerprint      TEXT,
  created_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- Link explorer_run -> battle_run_groups
ALTER TABLE explorer_run ADD COLUMN group_id INTEGER REFERENCES battle_run_groups(group_id);

-- Store probe on settings
ALTER TABLE explorer_settings ADD COLUMN seed_probe_id INTEGER REFERENCES seed_probe(id);

-- Settings <-> plan linkage (ordered vector of plans per settings)
CREATE TABLE IF NOT EXISTS explorer_settings_plan_link (
  settings_id INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  ordinal     INTEGER NOT NULL,
  plan_id     INTEGER NOT NULL REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  PRIMARY KEY (settings_id, ordinal)
);
CREATE INDEX IF NOT EXISTS ix_settings_plan_link_pid
  ON explorer_settings_plan_link(settings_id, plan_id);

-- Backfill plan links from the historical battle_plan.settings_id (if present)
INSERT INTO explorer_settings_plan_link(settings_id, ordinal, plan_id)
SELECT
  bp.settings_id,
  (SELECT COUNT(*) FROM battle_plan AS bp2
    WHERE bp2.settings_id = bp.settings_id AND bp2.plan_id <= bp.plan_id) - 1 AS ordinal,
  bp.plan_id
FROM battle_plan AS bp
WHERE EXISTS(SELECT 1 FROM pragma_table_info('battle_plan') WHERE name='settings_id')
ORDER BY bp.settings_id, bp.plan_id;

-- Drop index that referenced settings_id (if present)
DROP INDEX IF EXISTS idx_battle_plan_by_settings;

-- Rebuild battle_plan without settings_id, keep UNIQUE(fingerprint)
CREATE TABLE battle_plan__new (
  plan_id     INTEGER PRIMARY KEY,
  name        TEXT,
  fingerprint TEXT NOT NULL UNIQUE,
  num_turns   INTEGER NOT NULL,
  created_at  INTEGER NOT NULL
);

INSERT INTO battle_plan__new(plan_id, name, fingerprint, num_turns, created_at)
SELECT plan_id, name, fingerprint, num_turns, created_at
FROM battle_plan;

DROP TABLE battle_plan;
ALTER TABLE battle_plan__new RENAME TO battle_plan;

-- PredicateSpec fingerprint (dedupe across settings)
ALTER TABLE predicate_spec ADD COLUMN fingerprint TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS ux_predicate_spec_fingerprint
  ON predicate_spec(fingerprint)
  WHERE fingerprint IS NOT NULL;

COMMIT;
PRAGMA foreign_keys=ON;
