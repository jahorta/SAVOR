CREATE INDEX IF NOT EXISTS ix_ui_job_summary_job_set_job
    ON ui_job_summary(job_set_id, job_id);

CREATE INDEX IF NOT EXISTS ix_ui_workflow_step_job_set_instance
    ON ui_workflow_step(job_set_id, workflow_instance_id);
