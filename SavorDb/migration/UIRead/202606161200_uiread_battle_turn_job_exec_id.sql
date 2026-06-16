ALTER TABLE ui_battle_turn_job ADD COLUMN exec_job_id INTEGER NULL;

CREATE INDEX IF NOT EXISTS ix_ui_battle_turn_job_exec_job_id
    ON ui_battle_turn_job(exec_job_id);
