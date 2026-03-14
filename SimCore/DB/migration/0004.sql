-- 0004.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (4, strftime('%s','now'));

ALTER TABLE explorer_run
    ADD COLUMN status TEXT NOT NULL DEFAULT 'planned'
    CHECK (status IN ('planned','running','done'));

CREATE INDEX IF NOT EXISTS ix_explorer_run_active_status
    ON explorer_run(status)
    WHERE status IN ('planned','running');

COMMIT;