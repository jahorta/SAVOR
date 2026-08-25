ALTER TABLE ui_battle_group
ADD COLUMN continue_automatic_exploration_after_victory INTEGER NOT NULL DEFAULT 0
CHECK(continue_automatic_exploration_after_victory IN (0,1));
