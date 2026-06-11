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
    created_by TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL
, workflow_graph_revision_id INTEGER NULL);
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
    created_at_utc INTEGER NOT NULL, graph_node_key TEXT NULL, workflow_unit_activation_id INTEGER NULL REFERENCES exec_workflow_unit_activation(workflow_unit_activation_id),
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
CREATE TABLE exec_workflow_instance_input_binding (
    workflow_instance_input_binding_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_graph_revision_id INTEGER NOT NULL,
    node_key TEXT NOT NULL,
    input_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL,
    ref_id INTEGER NOT NULL,
    source_kind TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    CONSTRAINT uq_exec_workflow_instance_input_binding UNIQUE (workflow_instance_id, node_key, input_key)
);
CREATE INDEX ix_exec_workflow_instance_graph_revision
    ON exec_workflow_instance(workflow_graph_revision_id);
CREATE INDEX ix_exec_workflow_instance_input_binding_instance
    ON exec_workflow_instance_input_binding(workflow_instance_id, node_key, input_key);
CREATE TABLE exec_workflow_instance_argument (
    workflow_instance_argument_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    node_key TEXT NOT NULL DEFAULT '',
    argument_key TEXT NOT NULL,
    value_type TEXT NOT NULL CHECK(value_type IN ('integer','text','json','boolean')),
    integer_value INTEGER NULL,
    text_value TEXT NULL,
    source_kind TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    CONSTRAINT uq_exec_workflow_instance_argument UNIQUE (workflow_instance_id, node_key, argument_key)
);
CREATE INDEX ix_exec_workflow_instance_argument_instance
    ON exec_workflow_instance_argument(workflow_instance_id, node_key, argument_key);
CREATE TABLE exec_workflow_step_output (
    workflow_step_output_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    graph_node_key TEXT NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL,
    ref_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(workflow_step_id) REFERENCES exec_workflow_step(workflow_step_id),
    CONSTRAINT uq_exec_workflow_step_output UNIQUE (workflow_instance_id, graph_node_key, output_key)
);
CREATE INDEX ix_exec_workflow_step_graph_node
    ON exec_workflow_step(workflow_instance_id, graph_node_key);
CREATE INDEX ix_exec_workflow_step_output_instance
    ON exec_workflow_step_output(workflow_instance_id, graph_node_key, output_key);
CREATE TABLE exec_job_output (
    job_output_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL,
    ref_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(job_id) REFERENCES exec_job(job_id),
    CONSTRAINT uq_exec_job_output UNIQUE (job_id, output_key)
);
CREATE INDEX ix_exec_job_output_job
    ON exec_job_output(job_id, output_key);
CREATE TABLE exec_workflow_unit_activation (
    workflow_unit_activation_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    parent_workflow_unit_activation_id INTEGER NULL,
    activation_key TEXT NOT NULL,
    graph_node_key TEXT NOT NULL,
    unit_kind TEXT NOT NULL,
    display_name TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('WAITING','READY','RUNNING','COMPLETED','FAILED','SKIPPED','CANCELED')),
    activation_params_json TEXT NOT NULL DEFAULT '',
    authored_ref_kind TEXT NULL,
    authored_ref_id INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    ready_at_utc INTEGER NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failed_at_utc INTEGER NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(parent_workflow_unit_activation_id) REFERENCES exec_workflow_unit_activation(workflow_unit_activation_id),
    CONSTRAINT uq_exec_workflow_unit_activation_key UNIQUE (workflow_instance_id, activation_key)
);
CREATE TABLE exec_workflow_unit_activation_edge (
    workflow_unit_activation_edge_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    from_workflow_unit_activation_id INTEGER NOT NULL,
    to_workflow_unit_activation_id INTEGER NOT NULL,
    output_key TEXT NULL,
    input_key TEXT NULL,
    condition_kind TEXT NULL,
    condition_value TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(from_workflow_unit_activation_id) REFERENCES exec_workflow_unit_activation(workflow_unit_activation_id),
    FOREIGN KEY(to_workflow_unit_activation_id) REFERENCES exec_workflow_unit_activation(workflow_unit_activation_id),
    CONSTRAINT uq_exec_workflow_unit_activation_edge UNIQUE (workflow_instance_id, from_workflow_unit_activation_id, to_workflow_unit_activation_id, output_key, input_key)
);
CREATE INDEX ix_exec_workflow_unit_activation_instance_state
    ON exec_workflow_unit_activation(workflow_instance_id, state, created_at_utc ASC);
CREATE INDEX ix_exec_workflow_unit_activation_graph_node
    ON exec_workflow_unit_activation(workflow_instance_id, graph_node_key);
CREATE INDEX ix_exec_workflow_unit_activation_edge_instance_to
    ON exec_workflow_unit_activation_edge(workflow_instance_id, to_workflow_unit_activation_id);
CREATE INDEX ix_exec_workflow_step_activation
    ON exec_workflow_step(workflow_instance_id, workflow_unit_activation_id);
