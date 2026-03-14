-- 0026.sql - ui_config_rows for authoring template snapshots
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (26, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS ui_config_rows(
  id INTEGER PRIMARY KEY,
  preset_id INTEGER NOT NULL REFERENCES turn_action_presets(id) ON DELETE RESTRICT,
  turn_index INTEGER NOT NULL,
  actor_slot INTEGER NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS ux_ui_config_rows_preset ON ui_config_rows(preset_id);
CREATE INDEX IF NOT EXISTS ix_ui_config_rows_turnslot ON ui_config_rows(turn_index, actor_slot);

COMMIT;
