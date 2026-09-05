BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE exec_workflow_transition_activation (
    workflow_transition_activation_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    source_workflow_step_id INTEGER NOT NULL,
    activation_kind TEXT NOT NULL CHECK(activation_kind IN ('SETTLEMENT','MANUAL_BATTLE_CONTINUATION')),
    activation_key TEXT NOT NULL,
    trigger_fingerprint TEXT NOT NULL,
    decision_payload TEXT NOT NULL,
    decision_sha256 TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('FROZEN','APPLIED')),
    disposition TEXT NULL,
    last_operation_diagnostic TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    applied_at_utc INTEGER NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(source_workflow_step_id) REFERENCES exec_workflow_step(workflow_step_id),
    CONSTRAINT uq_exec_workflow_transition_activation_key
        UNIQUE(workflow_instance_id, activation_key)
);

CREATE INDEX ix_exec_workflow_transition_activation_pending
    ON exec_workflow_transition_activation(state, workflow_instance_id, source_workflow_step_id);

COMMIT;
