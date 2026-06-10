BEGIN IMMEDIATE;

ALTER TABLE exec_workflow_instance
    ADD COLUMN workflow_graph_revision_id INTEGER NULL;

CREATE TABLE IF NOT EXISTS exec_workflow_instance_input_binding (
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

CREATE INDEX IF NOT EXISTS ix_exec_workflow_instance_graph_revision
    ON exec_workflow_instance(workflow_graph_revision_id);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_instance_input_binding_instance
    ON exec_workflow_instance_input_binding(workflow_instance_id, node_key, input_key);

COMMIT;
