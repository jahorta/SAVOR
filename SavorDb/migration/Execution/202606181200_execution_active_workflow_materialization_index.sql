BEGIN IMMEDIATE;

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_state_instance
    ON exec_workflow_step(state, workflow_instance_id);

COMMIT;
