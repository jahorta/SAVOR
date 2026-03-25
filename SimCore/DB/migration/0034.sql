-- 0034: predicate multi-breakpoint support
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (34, strftime('%s','now'));

ALTER TABLE predicate_spec ADD COLUMN required_bp_multi TEXT;

COMMIT;
