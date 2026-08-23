BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS exec_workflow_unit_activation (
    workflow_unit_activation_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    parent_workflow_unit_activation_id INTEGER NULL,
    activation_key TEXT NOT NULL,
    graph_node_key TEXT NOT NULL,
    unit_kind TEXT NOT NULL,
    display_name TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('WAITING','READY','RUNNING','COMPLETED','FAILED','INTERRUPTED','SKIPPED','CANCELED')),
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

CREATE TABLE IF NOT EXISTS exec_workflow_unit_activation_edge (
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

ALTER TABLE exec_workflow_step
    ADD COLUMN workflow_unit_activation_id INTEGER NULL REFERENCES exec_workflow_unit_activation(workflow_unit_activation_id);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_unit_activation_instance_state
    ON exec_workflow_unit_activation(workflow_instance_id, state, created_at_utc ASC);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_unit_activation_graph_node
    ON exec_workflow_unit_activation(workflow_instance_id, graph_node_key);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_unit_activation_edge_instance_to
    ON exec_workflow_unit_activation_edge(workflow_instance_id, to_workflow_unit_activation_id);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_activation
    ON exec_workflow_step(workflow_instance_id, workflow_unit_activation_id);

COMMIT;
