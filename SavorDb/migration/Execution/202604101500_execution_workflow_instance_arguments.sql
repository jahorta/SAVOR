BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS exec_workflow_instance_argument (
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

CREATE INDEX IF NOT EXISTS ix_exec_workflow_instance_argument_instance
    ON exec_workflow_instance_argument(workflow_instance_id, node_key, argument_key);

COMMIT;
