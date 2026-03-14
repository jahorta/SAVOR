-- Migration 0013: drop deprecated wire_* and is_prelude from battle_plan_atom

PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (13, strftime('%s','now'));

DROP TABLE IF EXISTS battle_plan_atom;

CREATE TABLE battle_plan_atom (
  id             INTEGER PRIMARY KEY,
  action_type    INTEGER,
  actor_slot     INTEGER,
  param_item_id  INTEGER,
  target_slot    INTEGER
);

COMMIT;
PRAGMA foreign_keys=ON;
