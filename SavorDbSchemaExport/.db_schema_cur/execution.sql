CREATE TABLE migration_history (
    context TEXT NOT NULL,
    migration_name TEXT NOT NULL,
    applied_at_utc INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (context, migration_name)
);
CREATE TABLE migration_schema_version (
    context TEXT PRIMARY KEY,
    version INTEGER NOT NULL
);
CREATE TABLE exec_job_set (
    job_set_id INTEGER PRIMARY KEY,
    parent_job_set_id INTEGER NULL,
    program_kind INTEGER NOT NULL,
    purpose TEXT NOT NULL,
    created_by TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    priority_boost INTEGER NOT NULL DEFAULT 0,
    expected_total INTEGER NULL,
    domain_ref_kind TEXT NULL,
    domain_ref_id INTEGER NULL,
    meta_note TEXT NULL,
    FOREIGN KEY(parent_job_set_id) REFERENCES exec_job_set(job_set_id)
);
CREATE TABLE exec_job (
    job_id INTEGER PRIMARY KEY,
    job_set_id INTEGER NOT NULL,
    parent_job_id INTEGER NULL,
    program_kind INTEGER NOT NULL,
    program_version INTEGER NOT NULL,
    program_ref_kind TEXT NOT NULL,
    program_ref_id INTEGER NOT NULL,
    fingerprint TEXT NOT NULL,
    priority INTEGER NOT NULL,
    state TEXT NOT NULL,
    attempts INTEGER NOT NULL,
    max_attempts INTEGER NOT NULL,
    claimed_by_token TEXT NULL,
    lease_expires_at_utc INTEGER NULL,
    queued_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    ended_at_utc INTEGER NULL,
    error_code TEXT NULL,
    error_text TEXT NULL, savestate_id INTEGER NULL, input_ini TEXT NULL,
    FOREIGN KEY(job_set_id) REFERENCES exec_job_set(job_set_id),
    FOREIGN KEY(parent_job_id) REFERENCES exec_job(job_id),
    CONSTRAINT uq_exec_job_fingerprint UNIQUE (fingerprint)
);
CREATE TABLE exec_job_event (
    job_event_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    event_kind TEXT NOT NULL,
    event_ts_utc INTEGER NOT NULL,
    message TEXT NULL,
    artifact_id INTEGER NULL,
    FOREIGN KEY(job_id) REFERENCES exec_job(job_id)
);
CREATE TABLE exec_trigger (
    trigger_id INTEGER PRIMARY KEY,
    scope_kind TEXT NOT NULL CHECK(scope_kind IN ('job', 'job_set')),
    scope_id INTEGER NOT NULL,
    condition_kind TEXT NOT NULL,
    condition_value TEXT NOT NULL,
    action_kind TEXT NOT NULL,
    action_value TEXT NOT NULL,
    active INTEGER NOT NULL CHECK(active IN (0, 1)),
    created_at_utc INTEGER NOT NULL
);
CREATE TABLE exec_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NULL,
    causation_id TEXT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    CONSTRAINT uq_exec_outbox_event_id UNIQUE (event_id)
);
CREATE TABLE exec_archive_cursor (
    cursor_id INTEGER PRIMARY KEY,
    cursor_kind TEXT NOT NULL,
    last_scanned_at_utc INTEGER NOT NULL,
    last_job_set_id INTEGER NULL
);
CREATE INDEX ix_exec_job_parent_job_id
    ON exec_job(parent_job_id);
CREATE INDEX ix_exec_job_set_parent_job_set_id
    ON exec_job_set(parent_job_set_id);
CREATE INDEX ix_exec_job_claim_queue
    ON exec_job(state, priority DESC, queued_at_utc ASC);
CREATE INDEX ix_exec_job_job_set_state_queue_desc
    ON exec_job(job_set_id, state, queued_at_utc DESC);
CREATE INDEX ix_exec_outbox_unpublished
    ON exec_outbox_message(published_at_utc, outbox_id);
CREATE TABLE exec_workflow_instance (
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
CREATE TABLE exec_workflow_step (
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
CREATE TABLE exec_workflow_edge (
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
CREATE TABLE exec_workflow_event (
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
CREATE INDEX ix_exec_workflow_instance_state_created
    ON exec_workflow_instance(state, created_at_utc);
CREATE INDEX ix_exec_workflow_step_instance_state_priority_ready
    ON exec_workflow_step(workflow_instance_id, state, priority DESC, ready_at_utc ASC);
CREATE INDEX ix_exec_workflow_step_job_set
    ON exec_workflow_step(job_set_id);
CREATE INDEX ix_exec_workflow_edge_instance_to
    ON exec_workflow_edge(workflow_instance_id, to_step_id);
CREATE TABLE exec_workflow_input_event (
    workflow_input_event_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    event_kind TEXT NOT NULL,
    source_key TEXT NULL,
    request_id TEXT NULL,
    event_ts_utc INTEGER NOT NULL,
    detail_ref_kind TEXT NULL,
    detail_ref_id INTEGER NULL,
    message TEXT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(workflow_step_id) REFERENCES exec_workflow_step(workflow_step_id)
);
CREATE INDEX ix_exec_workflow_input_event_step_ts
    ON exec_workflow_input_event(workflow_step_id, event_ts_utc, workflow_input_event_id);
CREATE INDEX ix_exec_workflow_input_event_instance_step
    ON exec_workflow_input_event(workflow_instance_id, workflow_step_id, workflow_input_event_id);
CREATE INDEX ix_exec_outbox_payload_ref
    ON exec_outbox_message(payload_ref_kind, payload_ref_id, outbox_id);
CREATE INDEX ix_exec_outbox_replay_cursor
    ON exec_outbox_message(outbox_id, event_type);
CREATE INDEX ix_exec_job_savestate_id
    ON exec_job(savestate_id)
    WHERE savestate_id IS NOT NULL;
CREATE TABLE exec_handler_dedupe (
    dedupe_id INTEGER PRIMARY KEY,
    handler_name TEXT NOT NULL,
    event_id TEXT NULL,
    semantic_key TEXT NULL,
    first_seen_at_utc INTEGER NOT NULL,
    last_seen_at_utc INTEGER NOT NULL,
    CHECK (event_id IS NOT NULL OR semantic_key IS NOT NULL)
);
CREATE UNIQUE INDEX uq_exec_handler_dedupe_handler_event
    ON exec_handler_dedupe(handler_name, event_id)
    WHERE event_id IS NOT NULL;
CREATE UNIQUE INDEX uq_exec_handler_dedupe_handler_semantic
    ON exec_handler_dedupe(handler_name, semantic_key)
    WHERE semantic_key IS NOT NULL;
CREATE INDEX ix_exec_handler_dedupe_last_seen
    ON exec_handler_dedupe(last_seen_at_utc, dedupe_id);
