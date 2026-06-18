CREATE TABLE IF NOT EXISTS ui_battle_turn_job_replication (
    turn_job_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    wave_id INTEGER NOT NULL,
    parent_wave_id INTEGER NULL,
    parent_turn_job_id INTEGER NULL,
    exec_job_id INTEGER NULL,
    source_savestate_id INTEGER NULL,
    seed_candidate_id INTEGER NULL,
    authored_plan_id INTEGER NULL,
    authored_turn_index INTEGER NULL,
    resolved_turn_commands_blob TEXT NULL,
    resolved_turn_variant_key TEXT NULL,
    fake_attacks_used_before INTEGER NOT NULL DEFAULT 0,
    fake_attacks_this_turn INTEGER NOT NULL DEFAULT 0,
    output_savestate_id INTEGER NULL,
    input_trace_artifact_id INTEGER NULL
);

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_replication_parent
    ON ui_battle_turn_job_replication(parent_turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_replication_battle_set
    ON ui_battle_turn_job_replication(battle_set_id, authored_turn_index, turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_replication_source_savestate
    ON ui_battle_turn_job_replication(source_savestate_id);
