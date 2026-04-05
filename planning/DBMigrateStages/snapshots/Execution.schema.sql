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
    error_text TEXT NULL,
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
