BEGIN IMMEDIATE;

CREATE INDEX IF NOT EXISTS ix_exec_job_event_job_event_id
    ON exec_job_event(job_id, job_event_id);

COMMIT;
