BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS exec_workflow_instance (
    workflow_instance_id INTEGER PRIMARY KEY,
    workflow_kind TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('PENDING','RUNNING','COMPLETED','FAILED','CANCELED')),
    root_scope_kind TEXT NOT NULL CHECK(root_scope_kind IN ('job_set','run','manual')),
    root_scope_id INTEGER NULL,
    input_ref_kind TEXT NULL,
    input_ref_id INTEGER NULL,
    created_by TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL
);

CREATE TABLE IF NOT EXISTS exec_workflow_step (
    workflow_step_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    step_key TEXT NOT NULL,
    step_kind TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('WAITING','READY','MATERIALIZED','RUNNING','COMPLETED','FAILED','SKIPPED')),
    guard_kind TEXT NULL,
    guard_value TEXT NULL,
    priority INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    max_attempts INTEGER NOT NULL DEFAULT 1,
    job_set_id INTEGER NULL,
    input_ref_kind TEXT NULL,
    input_ref_id INTEGER NULL,
    output_ref_kind TEXT NULL,
    output_ref_id INTEGER NULL,
    blocked_reason TEXT NULL,
    ready_at_utc INTEGER NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failed_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(job_set_id) REFERENCES exec_job_set(job_set_id),
    CONSTRAINT uq_exec_workflow_step_instance_step_key UNIQUE (workflow_instance_id, step_key),
    CONSTRAINT uq_exec_workflow_step_job_set_id UNIQUE (job_set_id)
);

CREATE TABLE IF NOT EXISTS exec_workflow_edge (
    workflow_edge_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    from_step_id INTEGER NOT NULL,
    to_step_id INTEGER NOT NULL,
    condition_kind TEXT NULL,
    condition_value TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(from_step_id) REFERENCES exec_workflow_step(workflow_step_id),
    FOREIGN KEY(to_step_id) REFERENCES exec_workflow_step(workflow_step_id),
    CONSTRAINT uq_exec_workflow_edge_from_to UNIQUE (workflow_instance_id, from_step_id, to_step_id)
);

CREATE TABLE IF NOT EXISTS exec_workflow_event (
    workflow_event_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NULL,
    event_kind TEXT NOT NULL,
    event_ts_utc INTEGER NOT NULL,
    message TEXT NULL,
    detail_ref_kind TEXT NULL,
    detail_ref_id INTEGER NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(workflow_step_id) REFERENCES exec_workflow_step(workflow_step_id)
);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_instance_state_created
    ON exec_workflow_instance(state, created_at_utc);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_instance_state_priority_ready
    ON exec_workflow_step(workflow_instance_id, state, priority DESC, ready_at_utc ASC);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_job_set
    ON exec_workflow_step(job_set_id);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_edge_instance_to
    ON exec_workflow_edge(workflow_instance_id, to_step_id);

COMMIT;
