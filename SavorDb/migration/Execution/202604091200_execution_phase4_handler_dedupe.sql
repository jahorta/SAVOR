BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS exec_handler_dedupe (
    dedupe_id INTEGER PRIMARY KEY,
    handler_name TEXT NOT NULL,
    event_id TEXT NULL,
    semantic_key TEXT NULL,
    first_seen_at_utc INTEGER NOT NULL,
    last_seen_at_utc INTEGER NOT NULL,
    CHECK (event_id IS NOT NULL OR semantic_key IS NOT NULL)
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_handler_dedupe_handler_event
    ON exec_handler_dedupe(handler_name, event_id)
    WHERE event_id IS NOT NULL;

CREATE UNIQUE INDEX IF NOT EXISTS uq_exec_handler_dedupe_handler_semantic
    ON exec_handler_dedupe(handler_name, semantic_key)
    WHERE semantic_key IS NOT NULL;

CREATE INDEX IF NOT EXISTS ix_exec_handler_dedupe_last_seen
    ON exec_handler_dedupe(last_seen_at_utc, dedupe_id);

COMMIT;
