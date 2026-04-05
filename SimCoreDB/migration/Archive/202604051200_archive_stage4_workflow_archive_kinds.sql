BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS ar_archive_item_kind_catalog (
    item_kind TEXT PRIMARY KEY,
    include_by_default INTEGER NOT NULL CHECK(include_by_default IN (0, 1)),
    notes TEXT NULL
);

INSERT OR IGNORE INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES
    ('job_sets', 1, 'Execution job set rows'),
    ('jobs', 1, 'Execution job rows'),
    ('job_events', 1, 'Execution job event rows'),
    ('workflow_instances', 1, 'Workflow orchestration instance rows'),
    ('workflow_steps', 1, 'Workflow orchestration step rows'),
    ('workflow_edges', 1, 'Workflow orchestration edge rows'),
    ('workflow_events', 1, 'Workflow orchestration event rows'),
    ('triggers', 0, 'Legacy trigger rows for compatibility mode'),
    ('outbox', 0, 'Execution outbox rows for replay diagnostics');

COMMIT;
