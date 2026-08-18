-- Hard cut the authored workflow catalog to one entry-qualified SeedProbe unit.
-- Workflow instance history lives in Execution DB and is intentionally untouched.

CREATE TEMP TABLE au_adaptive_seed_probe_revisions (
    workflow_graph_revision_id INTEGER PRIMARY KEY
);

INSERT INTO au_adaptive_seed_probe_revisions(workflow_graph_revision_id)
SELECT DISTINCT workflow_graph_revision_id
FROM au_workflow_graph_revision_node
WHERE unit_kind IN (
    'seed_probe_chain',
    'battle_seed_probe',
    'dungeon_seed_probe',
    'overworld_seed_probe'
);

UPDATE au_workflow_graph_revision
SET graph_hash = 'adaptive-seedprobe-v1:' || graph_hash
WHERE workflow_graph_revision_id IN (
    SELECT workflow_graph_revision_id
    FROM au_adaptive_seed_probe_revisions
);

UPDATE au_workflow_graph_revision_node
SET unit_kind = 'seed_probe',
    display_name = 'SeedProbe'
WHERE unit_kind IN (
    'seed_probe_chain',
    'battle_seed_probe',
    'dungeon_seed_probe',
    'overworld_seed_probe'
);

DROP TABLE au_adaptive_seed_probe_revisions;
