-- 0025: GUI builder authoring templates (INI)
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (25, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS authoring_templates (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    seed_probe_id INTEGER NULL,
    ui_config_ini TEXT NOT NULL,
    predicate_specs_ini TEXT NOT NULL,
    fake_attack_budget INTEGER NOT NULL,
    last_materialized_settings_id INTEGER NULL,
    last_codec_version_seen INTEGER NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE UNIQUE INDEX IF NOT EXISTS ux_authoring_templates_name ON authoring_templates(LOWER(name));
COMMIT;
