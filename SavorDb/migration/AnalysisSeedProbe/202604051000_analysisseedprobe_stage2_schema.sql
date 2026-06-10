BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS sp_probe_set (
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

CREATE TABLE IF NOT EXISTS an_input_set (
    input_set_id INTEGER PRIMARY KEY,
    content_hash TEXT NULL,
    source_ref_kind TEXT NULL,
    source_ref_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_an_input_set_content_hash UNIQUE (content_hash)
);

CREATE UNIQUE INDEX IF NOT EXISTS ux_an_input_set_source
    ON an_input_set(source_ref_kind, source_ref_id)
    WHERE source_ref_kind IS NOT NULL AND source_ref_id IS NOT NULL;

CREATE TABLE IF NOT EXISTS sp_probe_run (
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

CREATE TABLE IF NOT EXISTS sp_probe_result (
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

CREATE TABLE IF NOT EXISTS sp_axis_xy (
    axis_xy_id INTEGER PRIMARY KEY,
    x INTEGER NOT NULL CHECK(x BETWEEN 0 AND 255),
    y INTEGER NOT NULL CHECK(y BETWEEN 0 AND 255),
    CONSTRAINT uq_sp_axis_xy_xy UNIQUE (x, y)
);

CREATE TABLE IF NOT EXISTS sp_input_frame (
    input_frame_id INTEGER PRIMARY KEY,
    main_axis_xy_id INTEGER NOT NULL,
    cstick_axis_xy_id INTEGER NOT NULL,
    trigger_axis_xy_id INTEGER NOT NULL,
    FOREIGN KEY(main_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    FOREIGN KEY(cstick_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    FOREIGN KEY(trigger_axis_xy_id) REFERENCES sp_axis_xy(axis_xy_id),
    CONSTRAINT uq_sp_input_frame_axes UNIQUE (main_axis_xy_id, cstick_axis_xy_id, trigger_axis_xy_id)
);

CREATE TABLE IF NOT EXISTS an_input_set_frame (
    input_set_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    input_frame_id INTEGER NOT NULL,
    added_at_utc INTEGER NOT NULL,
    PRIMARY KEY(input_set_id, ordinal),
    FOREIGN KEY(input_set_id) REFERENCES an_input_set(input_set_id),
    FOREIGN KEY(input_frame_id) REFERENCES sp_input_frame(input_frame_id)
);

CREATE TABLE IF NOT EXISTS sp_neutral_seed (
    neutral_seed_id INTEGER PRIMARY KEY,
    probe_result_id INTEGER NOT NULL,
    neutral_seed_value INTEGER NOT NULL,
    source_kind TEXT NOT NULL CHECK(source_kind IN ('CALCULATED', 'IMPORTED')),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_result_id) REFERENCES sp_probe_result(probe_result_id)
);

CREATE TABLE IF NOT EXISTS sp_grid_seed (
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

CREATE TABLE IF NOT EXISTS sp_unique_seed (
    unique_seed_id INTEGER PRIMARY KEY,
    probe_result_id INTEGER NOT NULL,
    input_frame_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL,
    seed_delta INTEGER NOT NULL,
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_result_id) REFERENCES sp_probe_result(probe_result_id),
    FOREIGN KEY(input_frame_id) REFERENCES sp_input_frame(input_frame_id)
);

CREATE TABLE IF NOT EXISTS sp_encounter_projection (
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

CREATE TABLE IF NOT EXISTS sp_outbox_message (
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

CREATE INDEX IF NOT EXISTS ix_sp_probe_run_probe_set_status
    ON sp_probe_run(probe_set_id, status, requested_at_utc DESC);

CREATE INDEX IF NOT EXISTS ix_sp_grid_seed_probe_result_family
    ON sp_grid_seed(probe_result_id, source_family, axis_xy_id);

CREATE INDEX IF NOT EXISTS ix_sp_unique_seed_probe_result
    ON sp_unique_seed(probe_result_id, input_frame_id);

CREATE INDEX IF NOT EXISTS ix_an_input_set_frame_input_set
    ON an_input_set_frame(input_set_id, ordinal);

CREATE INDEX IF NOT EXISTS ix_sp_outbox_unpublished
    ON sp_outbox_message(published_at_utc, outbox_id);

COMMIT;
