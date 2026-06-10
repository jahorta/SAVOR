BEGIN IMMEDIATE;

ALTER TABLE exec_job
    ADD COLUMN savestate_id INTEGER NULL;

CREATE INDEX IF NOT EXISTS ix_exec_job_savestate_id
    ON exec_job(savestate_id)
    WHERE savestate_id IS NOT NULL;

COMMIT;
