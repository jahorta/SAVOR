-- 0023: battle_contexts add savestate_id + codec_version
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (23, strftime('%s','now'));

ALTER TABLE battle_contexts DROP COLUMN bc_version;

ALTER TABLE battle_contexts ADD COLUMN codec_version INTEGER NOT NULL DEFAULT 1;

CREATE INDEX IF NOT EXISTS ix_battle_contexts_savestate_created ON battle_contexts(savestate_id, created_at DESC);
COMMIT;