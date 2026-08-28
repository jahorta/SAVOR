BEGIN IMMEDIATE;

ALTER TABLE exec_workflow_step
    ADD COLUMN graph_node_key TEXT NULL;

UPDATE exec_workflow_step
SET graph_node_key = step_key
WHERE graph_node_key IS NULL;

CREATE TABLE IF NOT EXISTS exec_workflow_step_output (
    workflow_step_output_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    graph_node_key TEXT NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL,
    ref_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    FOREIGN KEY(workflow_step_id) REFERENCES exec_workflow_step(workflow_step_id),
    CONSTRAINT uq_exec_workflow_step_output UNIQUE (workflow_step_id, output_key)
);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_graph_node
    ON exec_workflow_step(workflow_instance_id, graph_node_key);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_step_output_instance
    ON exec_workflow_step_output(workflow_instance_id, graph_node_key, output_key);

CREATE TABLE IF NOT EXISTS exec_job_output (
    job_output_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL,
    ref_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(job_id) REFERENCES exec_job(job_id),
    CONSTRAINT uq_exec_job_output UNIQUE (job_id, output_key)
);

CREATE INDEX IF NOT EXISTS ix_exec_job_output_job
    ON exec_job_output(job_id, output_key);

COMMIT;
