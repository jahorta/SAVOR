-- 0016.sql: add temp_path hint for materialized objects
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (16, strftime('%s','now'));

ALTER TABLE object_ref ADD COLUMN temp_path TEXT;

COMMIT;
