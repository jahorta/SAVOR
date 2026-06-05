BEGIN IMMEDIATE;

ALTER TABLE au_predicate_spec
    ADD COLUMN breakpoint_id INTEGER NOT NULL DEFAULT 0;

UPDATE au_predicate_spec
SET breakpoint_id =
    CASE breakpoint_name
        WHEN 'OverworldInit' THEN 501
        WHEN 'StartTravelInputs' THEN 502
        WHEN 'RandomEncounter' THEN 503
        WHEN 'ReachedGoal' THEN 504
        WHEN 'BeforeRandSeedSet' THEN 101
        WHEN 'AfterRandSeedSet' THEN 102
        WHEN 'BattleInit' THEN 201
        WHEN 'BattleInitComplete' THEN 202
        WHEN 'TurnInputs' THEN 203
        WHEN 'TurnIsReady' THEN 204
        WHEN 'StartTurn' THEN 205
        WHEN 'StartAction' THEN 206
        WHEN 'EndAction' THEN 207
        WHEN 'EndTurn' THEN 208
        WHEN 'Battle_Victory' THEN 209
        WHEN 'Battle_Defeat' THEN 210
        WHEN 'BattleLoadComplete' THEN 211
        ELSE CAST(breakpoint_name AS INTEGER)
    END
WHERE breakpoint_id = 0;

COMMIT;
