BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS ui_workflow_instance (
    workflow_instance_id INTEGER PRIMARY KEY,
    workflow_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    root_scope_kind TEXT NOT NULL,
    root_scope_id INTEGER NULL,
    created_by TEXT NULL,
    blocked_step_count INTEGER NOT NULL DEFAULT 0,
    failed_step_count INTEGER NOT NULL DEFAULT 0,
    created_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL
);

CREATE TABLE IF NOT EXISTS ui_workflow_step (
    workflow_step_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    step_key TEXT NOT NULL,
    step_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    blocked_reason TEXT NULL,
    job_set_id INTEGER NULL,
    job_count INTEGER NOT NULL DEFAULT 0,
    job_completed_count INTEGER NOT NULL DEFAULT 0,
    job_failed_count INTEGER NOT NULL DEFAULT 0,
    priority INTEGER NOT NULL,
    attempts INTEGER NOT NULL,
    max_attempts INTEGER NOT NULL,
    ready_at_utc INTEGER NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failed_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_ui_workflow_step_instance_key UNIQUE (workflow_instance_id, step_key)
);

CREATE TABLE IF NOT EXISTS ui_workflow_edge (
    workflow_edge_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    from_step_id INTEGER NOT NULL,
    to_step_id INTEGER NOT NULL,
    condition_kind TEXT NULL,
    condition_value TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_ui_workflow_edge_from_to UNIQUE (workflow_instance_id, from_step_id, to_step_id)
);

CREATE TABLE IF NOT EXISTS ui_workflow_alert (
    workflow_alert_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NULL,
    alert_kind TEXT NOT NULL,
    alert_code TEXT NULL,
    message TEXT NOT NULL,
    is_active INTEGER NOT NULL CHECK(is_active IN (0, 1)),
    first_seen_at_utc INTEGER NOT NULL,
    last_seen_at_utc INTEGER NOT NULL,
    cleared_at_utc INTEGER NULL
);

CREATE INDEX IF NOT EXISTS ix_ui_workflow_instance_state_created
    ON ui_workflow_instance(state, created_at_utc DESC);

CREATE INDEX IF NOT EXISTS ix_ui_workflow_step_instance_state
    ON ui_workflow_step(workflow_instance_id, state, priority DESC, created_at_utc ASC);

CREATE INDEX IF NOT EXISTS ix_ui_workflow_alert_active
    ON ui_workflow_alert(is_active, alert_kind, last_seen_at_utc DESC);

COMMIT;
