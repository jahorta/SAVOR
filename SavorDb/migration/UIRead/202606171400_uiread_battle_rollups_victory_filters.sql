ALTER TABLE ui_battle_turn_job ADD COLUMN has_final_victory_outcome INTEGER NOT NULL DEFAULT 0 CHECK(has_final_victory_outcome IN (0, 1));

ALTER TABLE ui_battle_wave ADD COLUMN job_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_wave ADD COLUMN selected_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_wave ADD COLUMN desired_outcome_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_wave ADD COLUMN final_victory_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_wave ADD COLUMN failed_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_wave ADD COLUMN advancement_rank INTEGER NOT NULL DEFAULT 0 CHECK(advancement_rank IN (0, 1, 2));

ALTER TABLE ui_battle_group ADD COLUMN wave_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN turn_job_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN selected_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN desired_outcome_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN final_victory_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN failed_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN manual_followup_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_battle_group ADD COLUMN advancement_rank INTEGER NOT NULL DEFAULT 0 CHECK(advancement_rank IN (0, 1, 2));

ALTER TABLE ui_workflow_step ADD COLUMN battle_advancement_rank INTEGER NOT NULL DEFAULT 0 CHECK(battle_advancement_rank IN (0, 1, 2));
ALTER TABLE ui_workflow_step ADD COLUMN battle_desired_outcome_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_workflow_step ADD COLUMN battle_final_victory_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_workflow_step ADD COLUMN battle_selected_count INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ui_workflow_instance ADD COLUMN battle_advancement_rank INTEGER NOT NULL DEFAULT 0 CHECK(battle_advancement_rank IN (0, 1, 2));
ALTER TABLE ui_workflow_instance ADD COLUMN battle_desired_outcome_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_workflow_instance ADD COLUMN battle_final_victory_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_workflow_instance ADD COLUMN battle_selected_count INTEGER NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_wave
    ON ui_battle_turn_job(wave_id, turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_wave_victory
    ON ui_battle_turn_job(wave_id, has_final_victory_outcome, turn_job_id);

CREATE INDEX IF NOT EXISTS ix_ui_battle_group_created
    ON ui_battle_group(created_at_utc DESC, battle_set_id DESC);

CREATE INDEX IF NOT EXISTS ix_ui_workflow_instance_battle_victory
    ON ui_workflow_instance(battle_final_victory_count, state, workflow_kind, created_at_utc DESC, workflow_instance_id DESC);
