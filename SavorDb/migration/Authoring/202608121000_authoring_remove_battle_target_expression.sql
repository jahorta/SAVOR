BEGIN IMMEDIATE;

ALTER TABLE au_battle_plan_action_preset DROP COLUMN target_expr_ini;

COMMIT;
