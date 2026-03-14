-- 0024: reusable per-actor action presets
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (24, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS turn_action_presets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    macro INTEGER NOT NULL,
    target_kind INTEGER NOT NULL,
    item_id INTEGER NULL,
    mask_bits INTEGER NULL,
    single_slot INTEGER NULL,
    same_as_pc INTEGER NULL,
    flags INTEGER NOT NULL DEFAULT 0,
    target_expr_ini TEXT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
CREATE INDEX IF NOT EXISTS ix_turn_action_presets_name ON turn_action_presets(LOWER(name));
COMMIT;
