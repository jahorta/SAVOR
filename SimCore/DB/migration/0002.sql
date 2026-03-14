-- Migration 0002: partial indexes and lineage

BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (2, strftime('%s','now'));

-- Partial indexes for active rows
CREATE INDEX IF NOT EXISTS ix_probe_active
  ON seed_probe(savestate_id) WHERE complete=0;
CREATE INDEX IF NOT EXISTS ix_delta_active
  ON seed_delta(probe_id) WHERE complete=0;
CREATE INDEX IF NOT EXISTS ix_run_active
  ON explorer_run(probe_id, delta_id) WHERE complete=0;

-- Covering index for lineage lookups
CREATE INDEX IF NOT EXISTS ix_run_lineage
  ON explorer_run(settings_id, probe_id, delta_id);

-- View for lineage
CREATE VIEW IF NOT EXISTS v_lineage AS
SELECT r.id AS run_id,
       r.probe_id,
       r.delta_id,
       r.settings_id,
       p.savestate_id
FROM explorer_run r
JOIN seed_probe p ON r.probe_id = p.id;

COMMIT;
