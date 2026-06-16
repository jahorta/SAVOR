PRAGMA foreign_keys = OFF;

BEGIN IMMEDIATE;

CREATE TABLE au_seed_probe_grid_spec_new (
    seed_probe_grid_spec_id INTEGER PRIMARY KEY,
    min_value INTEGER NOT NULL,
    max_value INTEGER NOT NULL,
    cap_trigger_top INTEGER NOT NULL CHECK(cap_trigger_top IN (0, 1)),
    ignore_trigger_min_max INTEGER NOT NULL CHECK(ignore_trigger_min_max IN (0, 1))
);

INSERT INTO au_seed_probe_grid_spec_new(
    seed_probe_grid_spec_id,
    min_value,
    max_value,
    cap_trigger_top,
    ignore_trigger_min_max
)
SELECT
    seed_probe_grid_spec_id,
    min_value,
    max_value,
    cap_trigger_top,
    ignore_trigger_min_max
FROM au_seed_probe_grid_spec;

DROP TABLE au_seed_probe_grid_spec;

ALTER TABLE au_seed_probe_grid_spec_new RENAME TO au_seed_probe_grid_spec;

CREATE TABLE au_tas_spec_base_new (
    tas_spec_base_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    progress_enable INTEGER NOT NULL CHECK(progress_enable IN (0, 1)),
    auto_queue_seeds INTEGER NOT NULL CHECK(auto_queue_seeds IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_tas_spec_base_name UNIQUE (name)
);

INSERT INTO au_tas_spec_base_new(
    tas_spec_base_id,
    name,
    priority,
    run_ms,
    vi_stall_ms,
    progress_enable,
    auto_queue_seeds,
    created_at_utc
)
SELECT
    tas_spec_base_id,
    name,
    priority,
    run_ms,
    vi_stall_ms,
    progress_enable,
    auto_queue_seeds,
    created_at_utc
FROM au_tas_spec_base;

DROP TABLE au_tas_spec_base;

ALTER TABLE au_tas_spec_base_new RENAME TO au_tas_spec_base;

CREATE TABLE au_battle_run_spec_new (
    battle_run_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    priority INTEGER NOT NULL,
    run_ms INTEGER NOT NULL,
    vi_stall_ms INTEGER NOT NULL,
    progress_enable INTEGER NOT NULL CHECK(progress_enable IN (0, 1)),
    use_single_turn_runner INTEGER NOT NULL CHECK(use_single_turn_runner IN (0, 1)),
    auto_wave_trigger_enable INTEGER NOT NULL CHECK(auto_wave_trigger_enable IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_battle_run_spec_name UNIQUE (name)
);

INSERT INTO au_battle_run_spec_new(
    battle_run_spec_id,
    name,
    priority,
    run_ms,
    vi_stall_ms,
    progress_enable,
    use_single_turn_runner,
    auto_wave_trigger_enable,
    created_at_utc
)
SELECT
    battle_run_spec_id,
    name,
    priority,
    run_ms,
    vi_stall_ms,
    progress_enable,
    use_single_turn_runner,
    auto_wave_trigger_enable,
    created_at_utc
FROM au_battle_run_spec;

DROP TABLE au_battle_run_spec;

ALTER TABLE au_battle_run_spec_new RENAME TO au_battle_run_spec;

COMMIT;

PRAGMA foreign_keys = ON;
