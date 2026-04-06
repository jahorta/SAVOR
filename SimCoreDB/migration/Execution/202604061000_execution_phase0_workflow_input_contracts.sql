BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS exec_workflow_input_event (
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

CREATE INDEX IF NOT EXISTS ix_exec_workflow_input_event_step_ts
    ON exec_workflow_input_event(workflow_step_id, event_ts_utc, workflow_input_event_id);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_input_event_instance_step
    ON exec_workflow_input_event(workflow_instance_id, workflow_step_id, workflow_input_event_id);

CREATE INDEX IF NOT EXISTS ix_exec_outbox_payload_ref
    ON exec_outbox_message(payload_ref_kind, payload_ref_id, outbox_id);

CREATE INDEX IF NOT EXISTS ix_exec_outbox_replay_cursor
    ON exec_outbox_message(outbox_id, event_type);

COMMIT;
