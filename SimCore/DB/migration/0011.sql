-- 0011.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (11, strftime('%s','now'));

ALTER TABLE jobs ADD COLUMN vm_kv TEXT NULL;

COMMIT;