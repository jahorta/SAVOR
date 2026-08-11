BEGIN IMMEDIATE;

CREATE TABLE ab_battle_start (
    battle_start_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    probe_run_id INTEGER NOT NULL,
    context_probe_id INTEGER NOT NULL,
    battle_set_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    battle_run_spec_id INTEGER NOT NULL,
    explorer_settings_id INTEGER NOT NULL,
    launch_fake_attack_min INTEGER NOT NULL,
    launch_fake_attack_max INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_run_id) REFERENCES sp_probe_run(probe_run_id),
    FOREIGN KEY(context_probe_id) REFERENCES ab_battle_context_probe(context_probe_id),
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id),
    CONSTRAINT uq_ab_battle_start_workflow_step UNIQUE(workflow_step_id),
    CONSTRAINT uq_ab_battle_start_battle_set UNIQUE(battle_set_id)
);

CREATE UNIQUE INDEX uq_ab_seed_candidate_probe_source
    ON ab_seed_candidate(battle_set_id, source_probe_result_id)
    WHERE source_probe_result_id IS NOT NULL;

CREATE UNIQUE INDEX uq_ab_turn_wave_first_seed
    ON ab_turn_wave(battle_set_id, turn_index, seed_candidate_id)
    WHERE parent_wave_id IS NULL AND parent_turn_job_id IS NULL;

CREATE UNIQUE INDEX uq_ab_turn_wave_parent_job
    ON ab_turn_wave(parent_wave_id, parent_turn_job_id, turn_index)
    WHERE parent_wave_id IS NOT NULL AND parent_turn_job_id IS NOT NULL;

COMMIT;
