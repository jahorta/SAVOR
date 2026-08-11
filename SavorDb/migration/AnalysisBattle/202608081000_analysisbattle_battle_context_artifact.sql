BEGIN IMMEDIATE;

ALTER TABLE ab_battle_context_probe
    ADD COLUMN materialization_key TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN workflow_instance_id INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN workflow_step_id INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN source_savestate_artifact_id INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN source_savestate_sha256 TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN full_phase_program_kind INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN full_phase_program_version INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN full_phase_canonical_id TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN full_phase_contract_revision INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN full_phase_sha256 TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN module_canonical_id TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN module_revision INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN module_sha256 TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN context_artifact_id INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN worker_terminal_sha256 TEXT NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN entry_pc INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN entry_vi_count INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN entry_epoch INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN capture_pc INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN capture_vi_count INTEGER NULL;
ALTER TABLE ab_battle_context_probe
    ADD COLUMN capture_epoch INTEGER NULL;

CREATE UNIQUE INDEX uq_ab_battle_context_materialization_key
    ON ab_battle_context_probe(materialization_key)
    WHERE materialization_key IS NOT NULL;
CREATE UNIQUE INDEX uq_ab_battle_context_workflow_step
    ON ab_battle_context_probe(workflow_step_id)
    WHERE workflow_step_id IS NOT NULL;
CREATE UNIQUE INDEX uq_ab_battle_context_worker_terminal
    ON ab_battle_context_probe(worker_terminal_sha256)
    WHERE worker_terminal_sha256 IS NOT NULL;

COMMIT;
