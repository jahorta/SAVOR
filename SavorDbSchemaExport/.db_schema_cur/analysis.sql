CREATE TABLE migration_history (
    context TEXT NOT NULL,
    migration_name TEXT NOT NULL,
    applied_at_utc INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (context, migration_name)
);
CREATE TABLE migration_schema_version (
    context TEXT PRIMARY KEY,
    version INTEGER NOT NULL
);
CREATE TABLE asp_run (
    run_id INTEGER PRIMARY KEY,
    run_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL
);
CREATE TABLE asp_state_ref (
    state_ref_id INTEGER PRIMARY KEY,
    run_id INTEGER NOT NULL,
    savestate_id INTEGER NOT NULL,
    role_kind TEXT NOT NULL CHECK(role_kind IN ('ENTRY', 'CHECKPOINT', 'OUTPUT')),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_lineage_edge (
    lineage_edge_id INTEGER PRIMARY KEY,
    parent_run_id INTEGER NOT NULL,
    child_run_id INTEGER NOT NULL,
    edge_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(parent_run_id) REFERENCES asp_run(run_id),
    FOREIGN KEY(child_run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_artifact_ref (
    artifact_ref_id INTEGER PRIMARY KEY,
    run_id INTEGER NOT NULL,
    artifact_id INTEGER NOT NULL,
    role_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(run_id) REFERENCES asp_run(run_id)
);
CREATE TABLE asp_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NULL,
    causation_id TEXT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    CONSTRAINT uq_asp_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_asp_outbox_unpublished
    ON asp_outbox_message(published_at_utc, outbox_id);
CREATE TABLE sp_probe_set (
    probe_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    probe_flavor TEXT NOT NULL CHECK(probe_flavor IN ('BATTLE_PRE', 'DUNGEON_PRE', 'OVERWORLD_PRE')),
    breakpoint_policy_name TEXT NOT NULL,
    dungeon_segment_file_num INTEGER NULL,
    dungeon_segment_file_letter TEXT NULL,
    dungeon_segment_code TEXT NULL,
    segment_source_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_sp_probe_set_name UNIQUE (name)
);
CREATE TABLE an_input_set (
    input_set_id INTEGER PRIMARY KEY,
    content_hash TEXT NULL,
    source_ref_kind TEXT NULL,
    source_ref_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_an_input_set_content_hash UNIQUE (content_hash)
);
CREATE UNIQUE INDEX ux_an_input_set_source
    ON an_input_set(source_ref_kind, source_ref_id)
    WHERE source_ref_kind IS NOT NULL AND source_ref_id IS NOT NULL;
CREATE TABLE sp_probe_run (
    probe_run_id INTEGER PRIMARY KEY,
    probe_set_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    seed_probe_spec_id INTEGER NOT NULL,
    codec_version INTEGER NOT NULL,
    status TEXT NOT NULL,
    unique_input_set_id INTEGER NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    FOREIGN KEY(probe_set_id) REFERENCES sp_probe_set(probe_set_id),
    FOREIGN KEY(unique_input_set_id) REFERENCES an_input_set(input_set_id)
);
CREATE TABLE sp_probe_result (
    probe_result_id INTEGER PRIMARY KEY,
    probe_run_id INTEGER NOT NULL,
    neutral_seed_value INTEGER NULL,
    grid_count INTEGER NOT NULL,
    unique_count INTEGER NOT NULL,
    result_status TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_run_id) REFERENCES sp_probe_run(probe_run_id),
    CONSTRAINT uq_sp_probe_result_probe_run UNIQUE (probe_run_id)
);
CREATE TABLE sp_axis_xy (
    axis_xy_id INTEGER PRIMARY KEY,
    x INTEGER NOT NULL CHECK(x BETWEEN 0 AND 255),
    y INTEGER NOT NULL CHECK(y BETWEEN 0 AND 255),
    CONSTRAINT uq_sp_axis_xy_xy UNIQUE (x, y)
);
CREATE TABLE sp_input_frame (
    input_frame_id INTEGER PRIMARY KEY,
    main_axis_xy_id INTEGER NOT NULL,
    cstick_axis_xy_id INTEGER NOT NULL,
    trigger_axis_xy_id INTEGER NOT NULL,
    FOREIGN KEY(main_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    FOREIGN KEY(cstick_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    FOREIGN KEY(trigger_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    CONSTRAINT uq_sp_input_frame_axes UNIQUE (main_axis_xy_id, cstick_axis_xy_id, trigger_axis_xy_id)
);
CREATE TABLE an_input_set_frame (
    input_set_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    input_frame_id INTEGER NOT NULL,
    added_at_utc INTEGER NOT NULL,
    PRIMARY KEY(input_set_id, ordinal),
    FOREIGN KEY(input_set_id) REFERENCES an_input_set(input_set_id),
    FOREIGN KEY(input_frame_id) REFERENCES sp_input_frame(input_frame_id)
);
CREATE TABLE sp_neutral_seed (
    neutral_seed_id INTEGER PRIMARY KEY,
    probe_result_id INTEGER NOT NULL,
    neutral_seed_value INTEGER NOT NULL,
    source_kind TEXT NOT NULL CHECK(source_kind IN ('CALCULATED', 'IMPORTED')),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_result_id) REFERENCES sp_probe_result(probe_result_id)
);
CREATE TABLE sp_grid_seed (
    grid_seed_id INTEGER PRIMARY KEY,
    probe_result_id INTEGER NOT NULL,
    source_family TEXT NOT NULL CHECK(source_family IN ('MAIN', 'CSTICK', 'TRIGGER')),
    axis_xy_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    seed_delta INTEGER NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_result_id) REFERENCES sp_probe_result(probe_result_id),
    FOREIGN KEY(axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id)
);
CREATE TABLE sp_unique_seed (
    unique_seed_id INTEGER PRIMARY KEY,
    probe_result_id INTEGER NOT NULL,
    input_frame_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    seed_delta INTEGER NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_result_id) REFERENCES sp_probe_result(probe_result_id),
    FOREIGN KEY(input_frame_id) REFERENCES sp_input_frame(input_frame_id)
);
CREATE TABLE sp_encounter_projection (
    encounter_projection_id INTEGER PRIMARY KEY,
    probe_run_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    option_ordinal INTEGER NOT NULL,
    encounter_id TEXT NOT NULL,
    encounter_frame INTEGER NOT NULL,
    stutter_step_at INTEGER NULL,
    movement_required INTEGER NOT NULL CHECK(movement_required IN (0, 1)),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_run_id) REFERENCES sp_probe_run(probe_run_id)
);
CREATE TABLE sp_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NULL,
    causation_id TEXT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    CONSTRAINT uq_sp_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_sp_probe_run_probe_set_status
    ON sp_probe_run(probe_set_id, status, requested_at_utc DESC);
CREATE INDEX ix_sp_grid_seed_probe_result_family
    ON sp_grid_seed(probe_result_id, source_family, axis_xy_id);
CREATE INDEX ix_sp_unique_seed_probe_result
    ON sp_unique_seed(probe_result_id, input_frame_id);
CREATE INDEX ix_an_input_set_frame_input_set
    ON an_input_set_frame(input_set_id, ordinal);
CREATE INDEX ix_sp_outbox_unpublished
    ON sp_outbox_message(published_at_utc, outbox_id);
CREATE TABLE ab_battle_set (
    battle_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    battle_run_spec_id INTEGER NOT NULL,
    explorer_settings_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_set_name UNIQUE (name)
);
CREATE TABLE ab_seed_candidate (
    seed_candidate_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    source_unique_seed_id INTEGER NULL,
    source_input_frame_id INTEGER NULL,
    seed_value INTEGER NOT NULL,
    source_kind TEXT NOT NULL CHECK(source_kind IN ('SP_UNIQUE', 'MANUAL', 'SYNTHETIC')),
    candidate_status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id)
);
CREATE TABLE ab_selection_pool (
    selection_pool_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    turn_index INTEGER NOT NULL,
    pool_name TEXT NOT NULL,
    criterion_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id)
);
CREATE TABLE ab_turn_wave (
    wave_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    turn_index INTEGER NOT NULL,
    context_probe_id INTEGER NULL,
    parent_wave_id INTEGER NULL,
    parent_turn_job_id INTEGER NULL,
    seed_candidate_id INTEGER NOT NULL,
    selection_pool_id INTEGER NULL,
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id),
    FOREIGN KEY(context_probe_id) REFERENCES ab_battle_context_probe(context_probe_id),
    FOREIGN KEY(parent_wave_id) REFERENCES ab_turn_wave(wave_id),
    FOREIGN KEY(parent_turn_job_id) REFERENCES ab_turn_job(turn_job_id),
    FOREIGN KEY(seed_candidate_id) REFERENCES ab_seed_candidate(seed_candidate_id),
    FOREIGN KEY(selection_pool_id) REFERENCES ab_selection_pool(selection_pool_id)
);
CREATE TABLE ab_battle_context_probe (
    context_probe_id INTEGER PRIMARY KEY,
    wave_id INTEGER NULL,
    source_savestate_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    probe_status TEXT NOT NULL,
    context_blob TEXT NULL,
    context_version INTEGER NULL,
    recorded_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id),
    CONSTRAINT uq_ab_battle_context_probe_exec_job_id UNIQUE (exec_job_id)
);
CREATE TABLE ab_turn_job (
    turn_job_id INTEGER PRIMARY KEY,
    wave_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    plan_id INTEGER NOT NULL,
    fake_attacks_this_turn INTEGER NOT NULL,
    fake_attacks_used_before INTEGER NOT NULL,
    job_state TEXT NOT NULL,
    started_at_utc INTEGER NULL,
    ended_at_utc INTEGER NULL,
    has_results INTEGER NOT NULL CHECK(has_results IN (0, 1)),
    vi_start INTEGER NULL,
    vi_end INTEGER NULL,
    delta_vi INTEGER NULL,
    rng_seed INTEGER NULL,
    battle_outcome INTEGER NULL,
    plan_materialize_err INTEGER NULL,
    pred_passed INTEGER NULL,
    pred_total INTEGER NULL,
    pred_abort_run INTEGER NULL,
    output_savestate_id INTEGER NULL,
    applied_input_artifact_id INTEGER NULL,
    recorded_at_utc INTEGER NULL, result_context_blob_base64 TEXT NULL, result_context_version INTEGER NULL,
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id),
    CONSTRAINT uq_ab_turn_job_exec_job_id UNIQUE (exec_job_id)
);
CREATE TABLE ab_selection_decision (
    selection_decision_id INTEGER PRIMARY KEY,
    selection_pool_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    decision_kind TEXT NOT NULL CHECK(decision_kind IN ('WINNER', 'DUPLICATE', 'REJECTED')),
    decision_reason TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(selection_pool_id) REFERENCES ab_selection_pool(selection_pool_id),
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);
CREATE TABLE ab_terminal_followup (
    terminal_followup_id INTEGER PRIMARY KEY,
    turn_job_id INTEGER NOT NULL,
    is_victory INTEGER NOT NULL CHECK(is_victory IN (0, 1)),
    manual_followup_status TEXT NOT NULL DEFAULT 'UNREVIEWED' CHECK(manual_followup_status IN ('UNREVIEWED', 'RECORDED')),
    recorded_dtm_artifact_id INTEGER NULL,
    recorded_dtmini_artifact_id INTEGER NULL,
    recorded_sav_artifact_id INTEGER NULL,
    note TEXT NULL,
    updated_at_utc INTEGER NOT NULL,
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id),
    CONSTRAINT uq_ab_terminal_followup_turn_job UNIQUE (turn_job_id),
    CONSTRAINT ck_ab_terminal_followup_recorded_artifact
        CHECK(manual_followup_status <> 'RECORDED' OR recorded_dtm_artifact_id IS NOT NULL)
);
CREATE TABLE ab_outbox_message (
    outbox_id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    context_name TEXT NOT NULL,
    aggregate_kind TEXT NOT NULL,
    aggregate_id TEXT NOT NULL,
    correlation_id TEXT NULL,
    causation_id TEXT NULL,
    occurred_at_utc INTEGER NOT NULL,
    payload_ref_kind TEXT NOT NULL,
    payload_ref_id INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NULL,
    CONSTRAINT uq_ab_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_ab_turn_wave_parent_wave_id
    ON ab_turn_wave(parent_wave_id);
CREATE INDEX ix_ab_turn_wave_parent_turn_job_id
    ON ab_turn_wave(parent_turn_job_id);
CREATE INDEX ix_ab_turn_wave_battle_set_turn
    ON ab_turn_wave(battle_set_id, turn_index);
CREATE INDEX ix_ab_battle_context_probe_wave_status
    ON ab_battle_context_probe(wave_id, probe_status);
CREATE INDEX ix_ab_battle_context_probe_savestate
    ON ab_battle_context_probe(source_savestate_id);
CREATE UNIQUE INDEX uq_ab_selection_pool_battle_turn_name
    ON ab_selection_pool(battle_set_id, turn_index, pool_name);
CREATE UNIQUE INDEX uq_ab_selection_decision_pool_turn_job
    ON ab_selection_decision(selection_pool_id, turn_job_id);
CREATE INDEX ix_ab_turn_job_wave_outcome
    ON ab_turn_job(wave_id, battle_outcome);
CREATE INDEX ix_ab_selection_decision_pool_kind
    ON ab_selection_decision(selection_pool_id, decision_kind);
CREATE INDEX ix_ab_terminal_followup_status_victory
    ON ab_terminal_followup(manual_followup_status, is_victory);
CREATE INDEX ix_ab_outbox_unpublished
    ON ab_outbox_message(published_at_utc, outbox_id);
