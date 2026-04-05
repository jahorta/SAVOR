CREATE TABLE au_seed_probe_spec (
    seed_probe_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    clear_result_winners INTEGER NOT NULL CHECK(clear_result_winners IN (0, 1)),
    samples_per_axis INTEGER NOT NULL,
    min_value INTEGER NOT NULL,
    max_value INTEGER NOT NULL,
    cap_trigger_top INTEGER NOT NULL CHECK(cap_trigger_top IN (0, 1)),
    ignore_trigger_minmax INTEGER NOT NULL CHECK(ignore_trigger_minmax IN (0, 1)),
    combo_attempts_per_target INTEGER NOT NULL,
    combo_sampler_tries INTEGER NOT NULL,
    auto_schedule_battle_run INTEGER NOT NULL CHECK(auto_schedule_battle_run IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_seed_probe_spec_name UNIQUE (name)
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
    macro INTEGER NOT NULL,
    target_kind INTEGER NOT NULL,
    target_slot INTEGER NULL,
    item_id INTEGER NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(plan_turn_id) REFERENCES au_battle_plan_turn(plan_turn_id)
);
CREATE TABLE au_predicate_spec (
    predicate_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    breakpoint_name TEXT NOT NULL,
    lhs_kind TEXT NOT NULL,
    lhs_value INTEGER NOT NULL,
    rhs_kind TEXT NOT NULL,
    rhs_value INTEGER NOT NULL,
    cmp_op TEXT NOT NULL,
    flag_mask INTEGER NULL,
    value_mask INTEGER NULL,
    abort_on_fail INTEGER NOT NULL CHECK(abort_on_fail IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_predicate_spec_name UNIQUE (name)
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
CREATE TABLE au_template (
    template_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    seed_probe_spec_id INTEGER NULL,
    tas_spec_id INTEGER NULL,
    battle_run_spec_id INTEGER NULL,
    explorer_settings_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(seed_probe_spec_id) REFERENCES au_seed_probe_spec(seed_probe_spec_id),
    FOREIGN KEY(tas_spec_id) REFERENCES au_tas_spec(tas_spec_id),
    FOREIGN KEY(battle_run_spec_id) REFERENCES au_battle_run_spec(battle_run_spec_id),
    FOREIGN KEY(explorer_settings_id) REFERENCES au_explorer_settings(explorer_settings_id),
    CONSTRAINT uq_au_template_name UNIQUE (name)
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
