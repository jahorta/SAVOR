CREATE TABLE ui_job_summary (
    job_id INTEGER PRIMARY KEY,
    job_set_id INTEGER NOT NULL,
    program_kind INTEGER NOT NULL,
    state TEXT NOT NULL,
    priority INTEGER NOT NULL,
    queued_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    ended_at_utc INTEGER NULL,
    error_code TEXT NULL
);
CREATE TABLE ui_job_detail (
    job_id INTEGER PRIMARY KEY,
    attempts INTEGER NOT NULL,
    max_attempts INTEGER NOT NULL,
    fingerprint TEXT NOT NULL,
    claimed_by_token TEXT NULL,
    lease_expires_at_utc INTEGER NULL,
    error_text TEXT NULL
);
CREATE TABLE ui_job_artifact (
    ui_job_artifact_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    artifact_id INTEGER NOT NULL,
    role_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_ui_job_artifact_job_artifact UNIQUE (job_id, artifact_id, role_kind)
);
CREATE TABLE ui_seed_probe_summary (
    probe_run_id INTEGER PRIMARY KEY,
    probe_set_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    neutral_seed_value INTEGER NULL,
    grid_count INTEGER NOT NULL,
    unique_count INTEGER NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL
);
CREATE TABLE ui_seed_probe_delta_point (
    delta_point_id INTEGER PRIMARY KEY,
    probe_run_id INTEGER NOT NULL,
    source_family TEXT NOT NULL,
    axis_x INTEGER NOT NULL,
    axis_y INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    seed_delta INTEGER NOT NULL,
    CONSTRAINT uq_ui_seed_probe_delta_point UNIQUE (probe_run_id, source_family, axis_x, axis_y, seed_value)
);
CREATE TABLE ui_seed_probe_unique_value (
    unique_value_id INTEGER PRIMARY KEY,
    probe_run_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    seed_delta INTEGER NOT NULL,
    main_x INTEGER NOT NULL,
    main_y INTEGER NOT NULL,
    cstick_x INTEGER NOT NULL,
    cstick_y INTEGER NOT NULL,
    trigger_x INTEGER NOT NULL,
    trigger_y INTEGER NOT NULL
);
CREATE TABLE ui_battle_group (
    battle_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL
);
CREATE TABLE ui_battle_wave (
    wave_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    parent_wave_id INTEGER NULL,
    turn_index INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL
);
CREATE TABLE ui_battle_turn_job (
    turn_job_id INTEGER PRIMARY KEY,
    wave_id INTEGER NOT NULL,
    job_state TEXT NOT NULL,
    fake_attacks_this_turn INTEGER NOT NULL,
    fake_attacks_used_before INTEGER NOT NULL,
    rng_seed INTEGER NULL,
    delta_vi INTEGER NULL,
    pred_passed INTEGER NULL,
    pred_total INTEGER NULL,
    battle_outcome INTEGER NULL,
    started_at_utc INTEGER NULL,
    ended_at_utc INTEGER NULL
);
CREATE TABLE ui_battle_followup (
    turn_job_id INTEGER PRIMARY KEY,
    is_victory INTEGER NOT NULL CHECK(is_victory IN (0, 1)),
    manual_followup_status TEXT NOT NULL,
    recorded_dtm_artifact_id INTEGER NULL,
    note TEXT NULL,
    updated_at_utc INTEGER NOT NULL
);
CREATE TABLE ui_artifact_browser (
    artifact_id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    artifact_kind TEXT NOT NULL,
    filename TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL
);
CREATE TABLE ui_archive_catalog (
    archive_package_id INTEGER PRIMARY KEY,
    source_context TEXT NOT NULL,
    source_job_set_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    schema_version INTEGER NOT NULL,
    event_catalog_version INTEGER NOT NULL,
    time_range_start_utc INTEGER NOT NULL,
    time_range_end_utc INTEGER NOT NULL,
    checksum_status TEXT NOT NULL
);
CREATE TABLE ui_projection_checkpoint (
    projector_name TEXT PRIMARY KEY,
    last_event_id TEXT NULL,
    last_outbox_id INTEGER NULL,
    updated_at_utc INTEGER NOT NULL
);
CREATE INDEX ix_ui_job_summary_list
    ON ui_job_summary(state, queued_at_utc DESC, priority DESC);
CREATE INDEX ix_ui_seed_probe_summary_list
    ON ui_seed_probe_summary(status, requested_at_utc DESC);
CREATE INDEX ix_ui_seed_probe_delta_probe_run
    ON ui_seed_probe_delta_point(probe_run_id, source_family);
CREATE INDEX ix_ui_battle_wave_group_turn
    ON ui_battle_wave(battle_set_id, turn_index, wave_id);
CREATE INDEX ix_ui_battle_followup_status
    ON ui_battle_followup(manual_followup_status, is_victory);
CREATE INDEX ix_ui_projection_checkpoint_outbox
    ON ui_projection_checkpoint(last_outbox_id);
