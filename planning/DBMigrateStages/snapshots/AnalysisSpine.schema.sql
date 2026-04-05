CREATE TABLE asp_run (
    run_id INTEGER PRIMARY KEY,
    run_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL
);
CREATE TABLE asp_state_ref (
    state_ref_id INTEGER PRIMARY KEY,
    run_id INTEGER NOT NULL,
    savestate_id INTEGER NOT NULL,
    role_kind TEXT NOT NULL CHECK(role_kind IN ('ENTRY', 'CHECKPOINT', 'OUTPUT')),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_lineage_edge (
    lineage_edge_id INTEGER PRIMARY KEY,
    parent_run_id INTEGER NOT NULL,
    child_run_id INTEGER NOT NULL,
    edge_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(parent_run_id) REFERENCES asp_run(run_id),
    FOREIGN KEY(child_run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_artifact_ref (
    artifact_ref_id INTEGER PRIMARY KEY,
    run_id INTEGER NOT NULL,
    artifact_id INTEGER NOT NULL,
    role_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_outbox_message (
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
    CONSTRAINT uq_asp_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_asp_outbox_unpublished
    ON asp_outbox_message(published_at_utc, outbox_id);
