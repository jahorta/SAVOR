BEGIN IMMEDIATE;

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS ab_battle_set (
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

CREATE TABLE IF NOT EXISTS ab_seed_candidate (
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

CREATE TABLE IF NOT EXISTS ab_selection_pool (
    selection_pool_id INTEGER PRIMARY KEY,
    battle_set_id INTEGER NOT NULL,
    turn_index INTEGER NOT NULL,
    pool_name TEXT NOT NULL,
    criterion_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id)
);

CREATE TABLE IF NOT EXISTS ab_turn_wave (
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

CREATE TABLE IF NOT EXISTS ab_battle_context_probe (
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

CREATE TABLE IF NOT EXISTS ab_turn_job (
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
    recorded_at_utc INTEGER NULL,
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id),
    CONSTRAINT uq_ab_turn_job_exec_job_id UNIQUE (exec_job_id)
);

CREATE TABLE IF NOT EXISTS ab_selection_decision (
    selection_decision_id INTEGER PRIMARY KEY,
    selection_pool_id INTEGER NOT NULL,
    turn_job_id INTEGER NOT NULL,
    decision_kind TEXT NOT NULL CHECK(decision_kind IN ('WINNER', 'DUPLICATE', 'REJECTED')),
    decision_reason TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(selection_pool_id) REFERENCES ab_selection_pool(selection_pool_id),
    FOREIGN KEY(turn_job_id) REFERENCES ab_turn_job(turn_job_id)
);

CREATE TABLE IF NOT EXISTS ab_terminal_followup (
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

CREATE TABLE IF NOT EXISTS ab_outbox_message (
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

CREATE INDEX IF NOT EXISTS ix_ab_turn_wave_parent_wave_id
    ON ab_turn_wave(parent_wave_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_wave_parent_turn_job_id
    ON ab_turn_wave(parent_turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_wave_battle_set_turn
    ON ab_turn_wave(battle_set_id, turn_index);

CREATE INDEX IF NOT EXISTS ix_ab_battle_context_probe_wave_status
    ON ab_battle_context_probe(wave_id, probe_status);

CREATE INDEX IF NOT EXISTS ix_ab_battle_context_probe_savestate
    ON ab_battle_context_probe(source_savestate_id);

CREATE UNIQUE INDEX IF NOT EXISTS uq_ab_selection_pool_battle_turn_name
    ON ab_selection_pool(battle_set_id, turn_index, pool_name);

CREATE UNIQUE INDEX IF NOT EXISTS uq_ab_selection_decision_pool_turn_job
    ON ab_selection_decision(selection_pool_id, turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ab_turn_job_wave_outcome
    ON ab_turn_job(wave_id, battle_outcome);

CREATE INDEX IF NOT EXISTS ix_ab_selection_decision_pool_kind
    ON ab_selection_decision(selection_pool_id, decision_kind);

CREATE INDEX IF NOT EXISTS ix_ab_terminal_followup_status_victory
    ON ab_terminal_followup(manual_followup_status, is_victory);

CREATE INDEX IF NOT EXISTS ix_ab_outbox_unpublished
    ON ab_outbox_message(published_at_utc, outbox_id);

COMMIT;
