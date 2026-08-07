BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS sp_probe_set (
    probe_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    probe_flavor TEXT NOT NULL CHECK(probe_flavor IN ('BATTLE_PRE', 'DUNGEON_PRE', 'OVERWORLD_PRE', 'FIELD_RETURN')),
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
    materialization_key TEXT NOT NULL
        CHECK(length(materialization_key) > 0),
    probe_set_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    seed_probe_spec_id INTEGER NOT NULL,
    launch_samples_per_axis INTEGER NOT NULL CHECK(launch_samples_per_axis > 0),
    codec_version INTEGER NOT NULL,
    status TEXT NOT NULL CHECK(status IN (
        'SURVEY',
        'SEARCH',
        'CONFIRM',
        'COMPLETED',
        'COMPLETED_PARTIAL',
        'FAILED',
        'INVALIDATED'
    )),
    accepted_input_set_id INTEGER NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    established_endpoint TEXT NULL CHECK(
        established_endpoint IS NULL OR established_endpoint IN (
            'AFTER_RAND_SEED_SET',
            'RAND_SEED_COMMITTED'
        )
    ),
    established_endpoint_source_job_id INTEGER NULL,
    conflicting_endpoint TEXT NULL CHECK(
        conflicting_endpoint IS NULL OR conflicting_endpoint IN (
            'AFTER_RAND_SEED_SET',
            'RAND_SEED_COMMITTED'
        )
    ),
    conflicting_endpoint_source_job_id INTEGER NULL,
    invalidation_diagnostic TEXT NULL,
    invalidated_at_utc INTEGER NULL,
    FOREIGN KEY(probe_set_id) REFERENCES sp_probe_set(probe_set_id),
    FOREIGN KEY(accepted_input_set_id) REFERENCES an_input_set(input_set_id),
    CONSTRAINT uq_sp_probe_run_materialization_key
        UNIQUE(materialization_key)
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

CREATE TABLE IF NOT EXISTS sp_probe_result (
    probe_result_id INTEGER PRIMARY KEY,
    probe_run_id INTEGER NOT NULL,
    input_frame_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL,
    seed_value INTEGER NOT NULL CHECK(seed_value BETWEEN 0 AND 4294967295),
    origin_worker_id INTEGER NOT NULL CHECK(origin_worker_id >= 0),
    origin_process_generation INTEGER NOT NULL CHECK(origin_process_generation > 0),
    origin_workset_epoch INTEGER NOT NULL CHECK(origin_workset_epoch > 0),
    terminal_sha256 TEXT NOT NULL CHECK(length(terminal_sha256) = 64),
    confirmation_of_probe_result_id INTEGER NULL,
    evidence_state TEXT NOT NULL CHECK(evidence_state IN (
        'OBSERVED',
        'PROVISIONAL',
        'CONFIRMED',
        'REJECTED'
    )),
    recorded_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_run_id) REFERENCES sp_probe_run(probe_run_id),
    FOREIGN KEY(input_frame_id) REFERENCES sp_input_frame(input_frame_id),
    FOREIGN KEY(confirmation_of_probe_result_id) REFERENCES sp_probe_result(probe_result_id),
    CONSTRAINT uq_sp_probe_result_source_job UNIQUE (source_job_id),
    CONSTRAINT ck_sp_probe_result_not_self_confirmation
        CHECK(confirmation_of_probe_result_id IS NULL OR confirmation_of_probe_result_id <> probe_result_id),
    CONSTRAINT ck_sp_probe_result_confirmation_is_observation
        CHECK(confirmation_of_probe_result_id IS NULL OR evidence_state = 'OBSERVED')
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

CREATE INDEX IF NOT EXISTS ix_sp_probe_result_run
    ON sp_probe_result(probe_run_id, probe_result_id);

CREATE INDEX IF NOT EXISTS ix_sp_probe_result_run_evidence
    ON sp_probe_result(probe_run_id, evidence_state, probe_result_id);

CREATE INDEX IF NOT EXISTS ix_sp_probe_result_run_frame
    ON sp_probe_result(probe_run_id, input_frame_id, probe_result_id);

CREATE UNIQUE INDEX IF NOT EXISTS ux_sp_probe_result_confirmation
    ON sp_probe_result(confirmation_of_probe_result_id)
    WHERE confirmation_of_probe_result_id IS NOT NULL;

CREATE UNIQUE INDEX IF NOT EXISTS ux_sp_probe_result_active_seed
    ON sp_probe_result(probe_run_id, seed_value)
    WHERE evidence_state IN ('PROVISIONAL', 'CONFIRMED');

CREATE TRIGGER IF NOT EXISTS tr_sp_probe_result_immutable_facts
BEFORE UPDATE OF
    probe_run_id,
    input_frame_id,
    source_job_id,
    seed_value,
    origin_worker_id,
    origin_process_generation,
    origin_workset_epoch,
    terminal_sha256,
    confirmation_of_probe_result_id,
    recorded_at_utc
ON sp_probe_result
BEGIN
    SELECT RAISE(ABORT, 'SeedProbe observation facts are immutable');
END;

CREATE TRIGGER IF NOT EXISTS tr_sp_probe_result_evidence_transition
BEFORE UPDATE OF evidence_state ON sp_probe_result
WHEN
    OLD.confirmation_of_probe_result_id IS NOT NULL
    OR NOT (
        (OLD.evidence_state = 'OBSERVED' AND NEW.evidence_state = 'PROVISIONAL')
        OR
        (OLD.evidence_state = 'PROVISIONAL' AND NEW.evidence_state IN ('CONFIRMED', 'REJECTED'))
    )
BEGIN
    SELECT RAISE(ABORT, 'invalid SeedProbe evidence transition');
END;

CREATE INDEX IF NOT EXISTS ix_an_input_set_frame_input_set
    ON an_input_set_frame(input_set_id, ordinal);

CREATE INDEX IF NOT EXISTS ix_sp_outbox_unpublished
    ON sp_outbox_message(published_at_utc, outbox_id);

COMMIT;
