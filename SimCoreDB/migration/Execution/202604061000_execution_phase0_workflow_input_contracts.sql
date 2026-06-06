BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE INDEX IF NOT EXISTS ix_exec_outbox_payload_ref
    ON exec_outbox_message(payload_ref_kind, payload_ref_id, outbox_id);

CREATE INDEX IF NOT EXISTS ix_exec_outbox_replay_cursor
    ON exec_outbox_message(outbox_id, event_type);

COMMIT;
