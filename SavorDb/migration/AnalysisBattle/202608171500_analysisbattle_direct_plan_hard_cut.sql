PRAGMA foreign_keys = OFF;
PRAGMA legacy_alter_table = ON;
BEGIN IMMEDIATE;

DELETE FROM ab_battle_replay;
DELETE FROM ab_battle_recording;
DELETE FROM ab_battle_completion;
DELETE FROM ab_predicate_execution_package_v1;
DELETE FROM ab_battle_single_turn_result_v1;
DELETE FROM ab_manual_followup;
DELETE FROM ab_battle_advancement_decision;
DELETE FROM ab_turn_job;
DELETE FROM ab_turn_wave;
DELETE FROM ab_battle_start;
DELETE FROM ab_seed_candidate;
DELETE FROM ab_battle_advancement_pool;
DELETE FROM ab_battle_context_probe;
DELETE FROM ab_battle_set;
DELETE FROM ab_outbox_message;

ALTER TABLE ab_battle_start RENAME TO ab_battle_start_legacy;
ALTER TABLE ab_battle_set RENAME TO ab_battle_set_legacy;

CREATE TABLE ab_battle_set (
    battle_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    battle_plan_id INTEGER NOT NULL,
    battle_plan_fingerprint TEXT NOT NULL CHECK(length(battle_plan_fingerprint)>0),
    continuation_mode TEXT NOT NULL CHECK(continuation_mode IN (
        'manual_selection','automatic_best_per_ending_rng')),
    launch_fake_attack_min INTEGER NOT NULL CHECK(launch_fake_attack_min>=0),
    launch_fake_attack_max INTEGER NOT NULL CHECK(launch_fake_attack_max>=launch_fake_attack_min),
    status TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    completed_at_utc INTEGER NULL,
    CONSTRAINT uq_ab_battle_set_name UNIQUE(name)
);

CREATE TABLE ab_battle_start (
    battle_start_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    probe_run_id INTEGER NOT NULL,
    context_probe_id INTEGER NOT NULL,
    battle_set_id INTEGER NOT NULL,
    entry_savestate_id INTEGER NOT NULL,
    battle_plan_id INTEGER NOT NULL,
    battle_plan_fingerprint TEXT NOT NULL,
    continuation_mode TEXT NOT NULL CHECK(continuation_mode IN (
        'manual_selection','automatic_best_per_ending_rng')),
    launch_fake_attack_min INTEGER NOT NULL,
    launch_fake_attack_max INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(probe_run_id) REFERENCES sp_probe_run(probe_run_id),
    FOREIGN KEY(context_probe_id) REFERENCES ab_battle_context_probe(context_probe_id),
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id),
    CONSTRAINT uq_ab_battle_start_workflow_step UNIQUE(workflow_step_id),
    CONSTRAINT uq_ab_battle_start_battle_set UNIQUE(battle_set_id)
);

DROP TABLE ab_battle_start_legacy;
DROP TABLE ab_battle_set_legacy;

COMMIT;
PRAGMA legacy_alter_table = OFF;
PRAGMA foreign_keys = ON;
