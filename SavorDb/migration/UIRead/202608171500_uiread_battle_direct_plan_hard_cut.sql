BEGIN IMMEDIATE;

DELETE FROM ui_battle_advancement_decision;
DELETE FROM ui_battle_manual_followup;
DELETE FROM ui_battle_turn_job_replication;
DELETE FROM ui_battle_turn_job;
DELETE FROM ui_battle_wave;
DELETE FROM ui_battle_group;
DELETE FROM ui_battle_context_summary;

COMMIT;
