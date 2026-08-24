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
, entry_savestate_id INTEGER NOT NULL DEFAULT 0, seed_probe_spec_id INTEGER NOT NULL DEFAULT 0, codec_version INTEGER NOT NULL DEFAULT 0);
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
CREATE TABLE ui_projection_handled_event (
    projector_name TEXT NOT NULL,
    event_id TEXT NOT NULL,
    last_outbox_id INTEGER NOT NULL,
    handled_at_utc INTEGER NOT NULL,
    PRIMARY KEY (projector_name, event_id)
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
CREATE INDEX ix_ui_projection_handled_event_outbox
    ON ui_projection_handled_event(projector_name, last_outbox_id);
CREATE TABLE ui_workflow_instance (
    workflow_instance_id INTEGER PRIMARY KEY,
    workflow_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    root_scope_kind TEXT NOT NULL,
    root_scope_id INTEGER NULL,
    created_by TEXT NULL,
    blocked_step_count INTEGER NOT NULL DEFAULT 0,
    failed_step_count INTEGER NOT NULL DEFAULT 0,
    created_at_utc INTEGER NOT NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL
);
CREATE TABLE ui_workflow_step (
    workflow_step_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    step_key TEXT NOT NULL,
    step_kind TEXT NOT NULL,
    state TEXT NOT NULL,
    blocked_reason TEXT NULL,
    job_set_id INTEGER NULL,
    job_count INTEGER NOT NULL DEFAULT 0,
    job_completed_count INTEGER NOT NULL DEFAULT 0,
    job_failed_count INTEGER NOT NULL DEFAULT 0,
    priority INTEGER NOT NULL,
    attempts INTEGER NOT NULL,
    max_attempts INTEGER NOT NULL,
    ready_at_utc INTEGER NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failed_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL, workflow_unit_activation_id INTEGER NULL,
    CONSTRAINT uq_ui_workflow_step_instance_key UNIQUE (workflow_instance_id, step_key)
);
CREATE TABLE ui_workflow_edge (
    workflow_edge_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    from_step_id INTEGER NOT NULL,
    to_step_id INTEGER NOT NULL,
    condition_kind TEXT NULL,
    condition_value TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_ui_workflow_edge_from_to UNIQUE (workflow_instance_id, from_step_id, to_step_id)
);
CREATE TABLE ui_workflow_alert (
    workflow_alert_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NULL,
    alert_kind TEXT NOT NULL,
    alert_code TEXT NULL,
    message TEXT NOT NULL,
    is_active INTEGER NOT NULL CHECK(is_active IN (0, 1)),
    first_seen_at_utc INTEGER NOT NULL,
    last_seen_at_utc INTEGER NOT NULL,
    cleared_at_utc INTEGER NULL
);
CREATE INDEX ix_ui_workflow_instance_state_created
    ON ui_workflow_instance(state, created_at_utc DESC);
CREATE INDEX ix_ui_workflow_step_instance_state
    ON ui_workflow_step(workflow_instance_id, state, priority DESC, created_at_utc ASC);
CREATE INDEX ix_ui_workflow_alert_active
    ON ui_workflow_alert(is_active, alert_kind, last_seen_at_utc DESC);
CREATE TABLE ui_projection_subscription (
    projector_name TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    last_outbox_id INTEGER NOT NULL DEFAULT 0,
    last_event_id TEXT NULL,
    updated_at_utc INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'ACTIVE' CHECK(status IN ('ACTIVE', 'PAUSED', 'ERROR')),
    last_error TEXT NULL,
    PRIMARY KEY (projector_name, source_context, source_outbox_table)
);
CREATE INDEX ix_ui_projection_subscription_stream_status_outbox
    ON ui_projection_subscription(source_context, source_outbox_table, status, last_outbox_id);
CREATE INDEX ix_ui_projection_subscription_status_updated
    ON ui_projection_subscription(status, updated_at_utc DESC);
CREATE TABLE ui_projection_subscription_audit (
    subscription_audit_id INTEGER PRIMARY KEY,
    projector_name TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    from_outbox_id INTEGER NOT NULL,
    to_outbox_id INTEGER NOT NULL,
    processed_count INTEGER NOT NULL DEFAULT 0,
    failed_count INTEGER NOT NULL DEFAULT 0,
    recorded_at_utc INTEGER NOT NULL
);
CREATE INDEX ix_ui_projection_subscription_audit_stream_recorded
    ON ui_projection_subscription_audit(source_context, source_outbox_table, recorded_at_utc DESC);
CREATE TABLE ui_archive_rehydrate_request (
    rehydrate_request_id INTEGER PRIMARY KEY,
    archive_package_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    target_namespace TEXT NOT NULL,
    requested_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    error_text TEXT NULL
);
CREATE INDEX ix_ui_archive_rehydrate_request_recent
    ON ui_archive_rehydrate_request(requested_at_utc DESC, rehydrate_request_id DESC);
CREATE INDEX ix_ui_archive_rehydrate_request_status_recent
    ON ui_archive_rehydrate_request(status, requested_at_utc DESC, rehydrate_request_id DESC);
CREATE TABLE ui_workflow_unit_activation (
    workflow_unit_activation_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    parent_workflow_unit_activation_id INTEGER NULL,
    activation_key TEXT NOT NULL,
    graph_node_key TEXT NOT NULL,
    unit_kind TEXT NOT NULL,
    display_name TEXT NOT NULL,
    state TEXT NOT NULL,
    activation_params_json TEXT NOT NULL DEFAULT '',
    authored_ref_kind TEXT NULL,
    authored_ref_id INTEGER NULL,
    failure_code TEXT NULL,
    failure_text TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    ready_at_utc INTEGER NULL,
    started_at_utc INTEGER NULL,
    completed_at_utc INTEGER NULL,
    failed_at_utc INTEGER NULL,
    CONSTRAINT uq_ui_workflow_unit_activation_key UNIQUE (workflow_instance_id, activation_key)
);
CREATE TABLE ui_workflow_unit_activation_edge (
    workflow_unit_activation_edge_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    from_workflow_unit_activation_id INTEGER NOT NULL,
    to_workflow_unit_activation_id INTEGER NOT NULL,
    output_key TEXT NULL,
    input_key TEXT NULL,
    condition_kind TEXT NULL,
    condition_value TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_ui_workflow_unit_activation_edge UNIQUE (workflow_instance_id, from_workflow_unit_activation_id, to_workflow_unit_activation_id, output_key, input_key)
);
CREATE INDEX ix_ui_workflow_unit_activation_instance_state
    ON ui_workflow_unit_activation(workflow_instance_id, state, created_at_utc ASC);
CREATE INDEX ix_ui_workflow_step_activation
    ON ui_workflow_step(workflow_instance_id, workflow_unit_activation_id);
