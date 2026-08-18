BEGIN IMMEDIATE;

CREATE TABLE tmv_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL UNIQUE,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NOT NULL,
    causation_id TEXT NOT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL
);

CREATE INDEX ix_tmv_outbox_unpublished
    ON tmv_outbox_message(published_at_utc,outbox_id);

COMMIT;
