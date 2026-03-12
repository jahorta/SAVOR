-- 0033: remove fake_atk_count from battle_plan_turn and fake_attack_budget from authoring_templates
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (33, strftime('%s','now'));

CREATE TABLE battle_plan_turn__new (
  plan_id       INTEGER NOT NULL REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  turn_index    INTEGER NOT NULL,
  PRIMARY KEY(plan_id, turn_index)
);

INSERT INTO battle_plan_turn__new(plan_id, turn_index)
SELECT plan_id, turn_index
FROM battle_plan_turn;

DROP TABLE battle_plan_turn;
ALTER TABLE battle_plan_turn__new RENAME TO battle_plan_turn;

CREATE TABLE authoring_templates__new (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    seed_probe_id INTEGER NULL,
    ui_config_ini TEXT NOT NULL,
    predicate_specs_ini TEXT NOT NULL,
    last_materialized_settings_id INTEGER NULL,
    last_codec_version_seen INTEGER NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

INSERT INTO authoring_templates__new(
    id,name,description,seed_probe_id,ui_config_ini,predicate_specs_ini,last_materialized_settings_id,last_codec_version_seen,created_at,updated_at
)
SELECT
    id,name,description,seed_probe_id,ui_config_ini,predicate_specs_ini,last_materialized_settings_id,last_codec_version_seen,created_at,updated_at
FROM authoring_templates;

DROP TABLE authoring_templates;
ALTER TABLE authoring_templates__new RENAME TO authoring_templates;
CREATE UNIQUE INDEX IF NOT EXISTS ux_authoring_templates_name ON authoring_templates(LOWER(name));

PRAGMA foreign_keys=ON;
COMMIT;
