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
CREATE TABLE au_seed_probe_grid_spec (
    seed_probe_grid_spec_id INTEGER PRIMARY KEY,
    samples_per_axis INTEGER NOT NULL,
    min_value INTEGER NOT NULL,
    max_value INTEGER NOT NULL,
    cap_trigger_top INTEGER NOT NULL CHECK(cap_trigger_top IN (0, 1)),
    ignore_trigger_min_max INTEGER NOT NULL CHECK(ignore_trigger_min_max IN (0, 1))
);
CREATE TABLE au_seed_probe_unique_spec (
    seed_probe_unique_spec_id INTEGER PRIMARY KEY,
    combo_attempts_per_target INTEGER NOT NULL,
    combo_sampler_tries INTEGER NOT NULL
);
CREATE TABLE au_seed_probe_spec (
    seed_probe_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    grid_spec_id INTEGER NOT NULL,
    unique_spec_id INTEGER NOT NULL,
    auto_schedule_battle_run INTEGER NOT NULL CHECK(auto_schedule_battle_run IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(grid_spec_id) REFERENCES au_seed_probe_grid_spec(seed_probe_grid_spec_id),
    FOREIGN KEY(unique_spec_id) REFERENCES au_seed_probe_unique_spec(seed_probe_unique_spec_id),
    CONSTRAINT uq_au_seed_probe_spec_name UNIQUE (name)
);
CREATE TABLE au_input_set (
    input_set_id INTEGER PRIMARY KEY,
    name TEXT NULL,
    content_hash TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_input_set_content_hash UNIQUE (content_hash)
);
CREATE TABLE au_input_set_frame (
    input_set_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    main_x INTEGER NOT NULL CHECK(main_x BETWEEN 0 AND 255),
    main_y INTEGER NOT NULL CHECK(main_y BETWEEN 0 AND 255),
    cstick_x INTEGER NOT NULL CHECK(cstick_x BETWEEN 0 AND 255),
    cstick_y INTEGER NOT NULL CHECK(cstick_y BETWEEN 0 AND 255),
    trigger_x INTEGER NOT NULL CHECK(trigger_x BETWEEN 0 AND 255),
    trigger_y INTEGER NOT NULL CHECK(trigger_y BETWEEN 0 AND 255),
    PRIMARY KEY(input_set_id, ordinal),
    FOREIGN KEY(input_set_id) REFERENCES au_input_set(input_set_id)
);
CREATE TABLE au_tas_spec_base (
    tas_spec_base_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    headroom_x10 INTEGER NOT NULL,
    progress_enable INTEGER NOT NULL CHECK(progress_enable IN (0, 1)),
    auto_queue_seeds INTEGER NOT NULL CHECK(auto_queue_seeds IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_tas_spec_base_name UNIQUE (name)
);
CREATE TABLE au_tas_spec (
    tas_spec_id INTEGER PRIMARY KEY,
    tas_spec_base_id INTEGER NOT NULL,
    base_dtm_artifact_id INTEGER NOT NULL,
    rtc_low INTEGER NOT NULL,
    rtc_high INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(tas_spec_base_id) REFERENCES au_tas_spec_base(tas_spec_base_id)
);
CREATE TABLE au_battle_run_spec (
    battle_run_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    progress_enable INTEGER NOT NULL CHECK(progress_enable IN (0, 1)),
    use_single_turn_runner INTEGER NOT NULL CHECK(use_single_turn_runner IN (0, 1)),
    auto_wave_trigger_enable INTEGER NOT NULL CHECK(auto_wave_trigger_enable IN (0, 1)),
    min_fake_attacks INTEGER NOT NULL,
    max_fake_attacks INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_battle_run_spec_name UNIQUE (name)
);
CREATE TABLE au_battle_plan (
    plan_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    fingerprint TEXT NOT NULL,
    num_turns INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_battle_plan_name UNIQUE (name),
    CONSTRAINT uq_au_battle_plan_fingerprint UNIQUE (fingerprint)
);
CREATE TABLE au_battle_plan_turn (
    plan_turn_id INTEGER PRIMARY KEY,
    plan_id INTEGER NOT NULL,
    turn_index INTEGER NOT NULL,
    FOREIGN KEY(plan_id) REFERENCES au_battle_plan(plan_id),
    CONSTRAINT uq_au_battle_plan_turn UNIQUE (plan_id, turn_index)
);
CREATE TABLE au_battle_plan_action (
    plan_action_id INTEGER PRIMARY KEY,
    plan_turn_id INTEGER NOT NULL,
    actor_slot INTEGER NOT NULL,
    action_preset_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(plan_turn_id) REFERENCES au_battle_plan_turn(plan_turn_id),
    FOREIGN KEY(action_preset_id) REFERENCES au_battle_plan_action_preset(action_preset_id)
);
CREATE TABLE au_battle_plan_action_preset (
    action_preset_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    macro INTEGER NOT NULL,
    target_kind INTEGER NOT NULL,
    item_id INTEGER NULL,
    target_mask_bits INTEGER NULL,
    target_single_slot INTEGER NULL,
    target_same_as_actor_slot INTEGER NULL,
    target_expr_ini TEXT NULL,
    flags INTEGER NOT NULL DEFAULT 0,
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NULL
);
CREATE TABLE au_address_program (
    address_program_id INTEGER PRIMARY KEY,
    program_version INTEGER NOT NULL,
    prog_bytes BLOB NOT NULL,
    derived_buffer_version INTEGER NULL,
    derived_buffer_schema_hash TEXT NULL,
    soa_structs_hash TEXT NULL,
    description TEXT NULL
);
CREATE UNIQUE INDEX ux_au_address_program_nk
    ON au_address_program(
        program_version,
        prog_bytes,
        COALESCE(derived_buffer_version, 0),
        COALESCE(derived_buffer_schema_hash, ''),
        COALESCE(soa_structs_hash, '')
    );
CREATE TABLE au_predicate_set (
    predicate_set_id INTEGER PRIMARY KEY,
    created_at_utc INTEGER NOT NULL
);
CREATE TABLE au_predicate_set_item (
    predicate_set_id INTEGER NOT NULL,
    predicate_spec_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    PRIMARY KEY(predicate_set_id, ordinal),
    FOREIGN KEY(predicate_set_id) REFERENCES au_predicate_set(predicate_set_id),
    FOREIGN KEY(predicate_spec_id) REFERENCES au_predicate_spec(predicate_spec_id)
);
CREATE TABLE au_explorer_settings (
    explorer_settings_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    default_plan_id INTEGER NULL,
    default_predicate_set_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(default_plan_id) REFERENCES au_battle_plan(plan_id),
    FOREIGN KEY(default_predicate_set_id) REFERENCES au_predicate_set(predicate_set_id),
    CONSTRAINT uq_au_explorer_settings_name UNIQUE (name)
);
CREATE TABLE au_outbox_message (
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
    CONSTRAINT uq_au_outbox_event_id UNIQUE (event_id)
);
CREATE INDEX ix_au_outbox_unpublished
    ON au_outbox_message(published_at_utc, outbox_id);
CREATE TABLE au_workflow_graph (
    workflow_graph_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    active_revision_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL, hidden INTEGER NOT NULL DEFAULT 0 CHECK(hidden IN (0, 1)),
    CONSTRAINT uq_au_workflow_graph_name UNIQUE (name),
    FOREIGN KEY(active_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id)
);
CREATE TABLE au_workflow_graph_revision (
    workflow_graph_revision_id INTEGER PRIMARY KEY,
    workflow_graph_id INTEGER NOT NULL,
    graph_version INTEGER NOT NULL DEFAULT 1,
    graph_hash TEXT NOT NULL,
    parent_revision_id INTEGER NULL,
    status TEXT NOT NULL DEFAULT 'active',
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_id) REFERENCES au_workflow_graph(workflow_graph_id),
    FOREIGN KEY(parent_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    CONSTRAINT uq_au_workflow_graph_revision_version UNIQUE (workflow_graph_id, graph_version),
    CONSTRAINT uq_au_workflow_graph_revision_hash UNIQUE (graph_hash)
);
CREATE TABLE au_workflow_graph_revision_node (
    workflow_graph_revision_node_id INTEGER PRIMARY KEY,
    workflow_graph_revision_id INTEGER NOT NULL,
    node_key TEXT NOT NULL,
    unit_kind TEXT NOT NULL,
    display_name TEXT NULL,
    authored_ref_kind TEXT NULL,
    authored_ref_id INTEGER NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_key UNIQUE (workflow_graph_revision_id, node_key)
);
CREATE TABLE au_workflow_graph_revision_node_input (
    workflow_graph_revision_node_input_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    input_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    display_name TEXT NULL,
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_input UNIQUE (workflow_graph_revision_node_id, input_key)
);
CREATE TABLE au_workflow_graph_revision_node_output (
    workflow_graph_revision_node_output_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    display_name TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_output UNIQUE (workflow_graph_revision_node_id, output_key)
);
CREATE TABLE au_workflow_graph_revision_edge (
    workflow_graph_revision_edge_id INTEGER PRIMARY KEY,
    workflow_graph_revision_id INTEGER NOT NULL,
    from_revision_node_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    to_revision_node_id INTEGER NOT NULL,
    input_key TEXT NOT NULL,
    guard_kind TEXT NULL,
    guard_value TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_id) REFERENCES au_workflow_graph_revision(workflow_graph_revision_id),
    FOREIGN KEY(from_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    FOREIGN KEY(to_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_edge UNIQUE (workflow_graph_revision_id, from_revision_node_id, output_key, to_revision_node_id, input_key)
);
CREATE INDEX ix_au_workflow_graph_revision_graph
    ON au_workflow_graph_revision(workflow_graph_id, graph_version);
CREATE INDEX ix_au_workflow_graph_revision_node_revision
    ON au_workflow_graph_revision_node(workflow_graph_revision_id, ordinal);
CREATE INDEX ix_au_workflow_graph_revision_edge_revision
    ON au_workflow_graph_revision_edge(workflow_graph_revision_id, ordinal);
CREATE TABLE "au_predicate_spec" (
    predicate_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    breakpoint_name TEXT NOT NULL,
    breakpoint_id INTEGER NOT NULL,
    lhs_value INTEGER NOT NULL,
    rhs_value INTEGER NOT NULL,
    baseline_bps TEXT NOT NULL DEFAULT '',
    cmp_op TEXT NOT NULL,
    width INTEGER NOT NULL CHECK(width IN (1, 2, 4, 8)),
    flag_mask INTEGER NULL,
    value_mask INTEGER NULL,
    lhs_address_program_id INTEGER NULL,
    rhs_address_program_id INTEGER NULL,
    abort_on_fail INTEGER NOT NULL CHECK(abort_on_fail IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(lhs_address_program_id) REFERENCES au_address_program(address_program_id),
    FOREIGN KEY(rhs_address_program_id) REFERENCES au_address_program(address_program_id),
    CONSTRAINT uq_au_predicate_spec_name UNIQUE (name)
);
CREATE TABLE au_battle_chain_spec (
    battle_chain_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    battle_run_spec_id INTEGER NOT NULL,
    explorer_settings_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_run_spec_id) REFERENCES au_battle_run_spec(battle_run_spec_id),
    FOREIGN KEY(explorer_settings_id) REFERENCES au_explorer_settings(explorer_settings_id),
    CONSTRAINT uq_au_battle_chain_spec_name UNIQUE (name)
);
CREATE TABLE au_runtime_symbol_pack (
    runtime_symbol_pack_id INTEGER PRIMARY KEY,
    pack_id TEXT NOT NULL,
    schema_name TEXT NOT NULL,
    schema_version INTEGER NOT NULL,
    name TEXT NOT NULL,
    description TEXT NULL,
    content_hash TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    imported_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_runtime_symbol_pack UNIQUE(pack_id, schema_version),
    CONSTRAINT ck_au_runtime_symbol_pack_schema CHECK(schema_name = 'savor.runtime-symbol-pack'),
    CONSTRAINT ck_au_runtime_symbol_pack_version CHECK(schema_version = 1)
);
CREATE TABLE au_context_symbol (
    context_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    value_type TEXT NOT NULL,
    description TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_context_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_context_symbol_id CHECK(stable_id LIKE 'user.ctx.%'),
    CONSTRAINT ck_au_context_symbol_type CHECK(value_type IN ('u8','u16','u32','f32','f64','string','gc_input_frame','battle_path'))
);
CREATE TABLE au_address_symbol (
    address_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    region TEXT NOT NULL,
    base_address INTEGER NOT NULL,
    width INTEGER NULL,
    value_type TEXT NULL,
    notes TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_address_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_address_symbol_id CHECK(stable_id LIKE 'user.addr.%'),
    CONSTRAINT ck_au_address_symbol_region CHECK(region IN ('MEM1','MEM2','DERIVED')),
    CONSTRAINT ck_au_address_symbol_width CHECK(width IS NULL OR width IN (1,2,4,8)),
    CONSTRAINT ck_au_address_symbol_type CHECK(value_type IS NULL OR value_type IN ('u8','u16','u32','f32','f64','string','gc_input_frame','battle_path'))
);
CREATE TABLE au_breakpoint_symbol (
    breakpoint_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    address_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN (0, 1)),
    domain TEXT NULL,
    notes TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_breakpoint_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_breakpoint_symbol_id CHECK(stable_id LIKE 'user.bp.%'),
    CONSTRAINT ck_au_breakpoint_symbol_kind CHECK(kind = 'execute')
);
CREATE INDEX ix_au_context_symbol_pack
    ON au_context_symbol(runtime_symbol_pack_id, ordinal);
CREATE INDEX ix_au_address_symbol_pack
    ON au_address_symbol(runtime_symbol_pack_id, ordinal);
CREATE INDEX ix_au_breakpoint_symbol_pack
    ON au_breakpoint_symbol(runtime_symbol_pack_id, ordinal);
