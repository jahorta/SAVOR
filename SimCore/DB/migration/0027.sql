-- 0027.sql: Drop version_stamp and remove seed_probe.version_id

PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (27, strftime('%s','now'));

-- Drop dependent views that reference seed_probe so we can rebuild the table safely
DROP VIEW IF EXISTS v_lineage;

-- Rebuild seed_probe without the version_id column (which referenced version_stamp)
CREATE TABLE seed_probe__new (
  id             INTEGER PRIMARY KEY,
  savestate_id   INTEGER NOT NULL REFERENCES savestate(id),
  codec_version  INTEGER NOT NULL DEFAULT 0,
  neutral_seed   INTEGER,
  status         TEXT NOT NULL DEFAULT 'planned',
  complete       INTEGER NOT NULL DEFAULT 0
);

INSERT INTO seed_probe__new(id, savestate_id, neutral_seed, status, complete)
SELECT id, savestate_id, neutral_seed, status, complete
FROM seed_probe;

DROP TABLE seed_probe;
ALTER TABLE seed_probe__new RENAME TO seed_probe;

-- Recreate indexes that previous migrations defined for seed_probe
CREATE INDEX IF NOT EXISTS ix_probe_active          ON seed_probe(savestate_id) WHERE complete=0;
CREATE INDEX IF NOT EXISTS ix_seed_probe_status     ON seed_probe(status);
CREATE INDEX IF NOT EXISTS ix_seed_probe_savestate  ON seed_probe(savestate_id);

-- Recreate v_lineage (latest definition: explorer_run.delta_seed_id)
CREATE VIEW IF NOT EXISTS v_lineage AS
SELECT r.id AS run_id,
       r.probe_id,
       r.delta_seed_id,
       r.settings_id,
       p.savestate_id
FROM explorer_run r
JOIN seed_probe p ON r.probe_id = p.id;

-- Drop the now-unused version_stamp table
DROP TABLE IF EXISTS version_stamp;

COMMIT;
PRAGMA foreign_keys=ON;
