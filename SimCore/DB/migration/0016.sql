-- 0016.sql: add temp_path hint for materialized objects
BEGIN;

ALTER TABLE object_ref ADD COLUMN temp_path TEXT;

COMMIT;
