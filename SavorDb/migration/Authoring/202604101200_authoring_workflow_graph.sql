BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS au_workflow_graph (
    workflow_graph_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    active_revision_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_workflow_graph_name UNIQUE (name),
    FOREIGN KEY(active_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id)
);

CREATE TABLE IF NOT EXISTS au_workflow_graph_revision (
    workflow_graph_revision_id INTEGER PRIMARY KEY,
    workflow_graph_id INTEGER NOT NULL,
    graph_version INTEGER NOT NULL DEFAULT 1,
    graph_hash TEXT NOT NULL,
    parent_revision_id INTEGER NULL,
    status TEXT NOT NULL DEFAULT 'active',
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_id) REFERENCES au_workflow_graph(workflow_graph_id),
    FOREIGN KEY(parent_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    CONSTRAINT uq_au_workflow_graph_revision_version UNIQUE (workflow_graph_id, graph_version),
    CONSTRAINT uq_au_workflow_graph_revision_hash UNIQUE (graph_hash)
);

CREATE TABLE IF NOT EXISTS au_workflow_graph_revision_node (
    workflow_graph_revision_node_id INTEGER PRIMARY KEY,
    workflow_graph_revision_id INTEGER NOT NULL,
    node_key TEXT NOT NULL,
    unit_kind TEXT NOT NULL,
    display_name TEXT NULL,
    authored_ref_kind TEXT NULL,
    authored_ref_id INTEGER NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_key UNIQUE (workflow_graph_revision_id, node_key)
);

CREATE TABLE IF NOT EXISTS au_workflow_graph_revision_node_input (
    workflow_graph_revision_node_input_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    input_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    display_name TEXT NULL,
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_input UNIQUE (workflow_graph_revision_node_id, input_key)
);

CREATE TABLE IF NOT EXISTS au_workflow_graph_revision_node_output (
    workflow_graph_revision_node_output_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    display_name TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_output UNIQUE (workflow_graph_revision_node_id, output_key)
);

CREATE TABLE IF NOT EXISTS au_workflow_graph_revision_edge (
    workflow_graph_revision_edge_id INTEGER PRIMARY KEY,
    workflow_graph_revision_id INTEGER NOT NULL,
    from_revision_node_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    to_revision_node_id INTEGER NOT NULL,
    input_key TEXT NOT NULL,
    guard_kind TEXT NULL,
    guard_value TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    FOREIGN KEY(from_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    FOREIGN KEY(to_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_edge UNIQUE (workflow_graph_revision_id, from_revision_node_id, output_key, to_revision_node_id, input_key)
);

CREATE INDEX IF NOT EXISTS ix_au_workflow_graph_revision_graph
    ON au_workflow_graph_revision(workflow_graph_id, graph_version);

CREATE INDEX IF NOT EXISTS ix_au_workflow_graph_revision_node_revision
    ON au_workflow_graph_revision_node(workflow_graph_revision_id, ordinal);

CREATE INDEX IF NOT EXISTS ix_au_workflow_graph_revision_edge_revision
    ON au_workflow_graph_revision_edge(workflow_graph_revision_id, ordinal);

COMMIT;
