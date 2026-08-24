BEGIN IMMEDIATE;

ALTER TABLE ar_archive_package
    ADD COLUMN source_scope_kind TEXT NOT NULL DEFAULT 'job_set';

ALTER TABLE ar_archive_package
    ADD COLUMN source_workflow_count INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ar_archive_package
    ADD COLUMN selection_summary TEXT NULL;

CREATE TABLE IF NOT EXISTS ar_archive_workflow_package (
    archive_package_id INTEGER NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    workflow_kind TEXT NULL,
    display_state TEXT NULL,
    root_scope_kind TEXT NULL,
    root_scope_id INTEGER NULL,
    created_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    battle_final_victory_count INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(archive_package_id, workflow_instance_id),
    FOREIGN KEY(archive_package_id) REFERENCES ar_archive_package(archive_package_id)
);

CREATE INDEX IF NOT EXISTS ix_ar_archive_workflow_package_workflow
    ON ar_archive_workflow_package(workflow_instance_id, archive_package_id);

CREATE INDEX IF NOT EXISTS ix_ar_archive_package_scope
    ON ar_archive_package(source_scope_kind, created_at_utc DESC);

INSERT OR IGNORE INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES
    ('workflow_unit_activations', 1, 'Workflow unit activation rows'),
    ('workflow_unit_activation_edges', 1, 'Workflow unit activation edge rows'),
    ('workflow_step_outputs', 1, 'Workflow graph output rows'),
    ('workflow_instance_input_bindings', 1, 'Workflow input binding rows'),
    ('workflow_instance_arguments', 1, 'Workflow argument rows'),
    ('analysis_battle_sets', 1, 'Battle analysis set rows reachable from archived workflows'),
    ('analysis_seed_candidates', 1, 'Battle seed candidate rows reachable from archived workflows'),
    ('analysis_selection_pools', 1, 'Battle selection pool rows reachable from archived workflows'),
    ('analysis_turn_waves', 1, 'Battle turn wave rows reachable from archived workflows'),
    ('analysis_battle_context_probes', 1, 'Battle context probe rows reachable from archived workflows'),
    ('analysis_battle_turn_jobs', 1, 'Battle turn job rows reachable from archived workflows'),
    ('analysis_selection_decisions', 1, 'Battle selection decision rows reachable from archived workflows'),
    ('analysis_terminal_followups', 1, 'Battle terminal follow-up rows reachable from archived workflows'),
    ('ui_workflow_instances', 1, 'UIRead workflow snapshot rows'),
    ('ui_workflow_steps', 1, 'UIRead workflow step snapshot rows'),
    ('ui_workflow_edges', 1, 'UIRead workflow edge snapshot rows'),
    ('ui_workflow_alerts', 1, 'UIRead workflow alert snapshot rows'),
    ('ui_battle_turn_job_replications', 1, 'UIRead battle turn job replication snapshot rows'),
    ('state_artifacts', 1, 'State DB artifact metadata rows for archived SAV payloads'),
    ('state_savestates', 1, 'State DB savestate metadata rows'),
    ('state_savestate_derivations', 1, 'State DB savestate derivation rows among archived savestates'),
    ('state_savestate_zip', 1, 'SAV file bytes stored in savestates.zip');

COMMIT;
