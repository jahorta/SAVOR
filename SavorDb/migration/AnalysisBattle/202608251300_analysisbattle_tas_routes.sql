BEGIN IMMEDIATE;

CREATE TABLE atr_route_node (
    route_node_id INTEGER PRIMARY KEY,
    parent_route_node_id INTEGER NULL,
    node_kind TEXT NOT NULL CHECK(node_kind IN ('CHECKPOINT','ACTIVITY')),
    activity_kind TEXT NOT NULL DEFAULT '',
    activity_key TEXT NOT NULL DEFAULT '',
    label TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    source_dtm_artifact_id INTEGER NULL,
    source_savestate_id INTEGER NULL,
    battle_plan_id INTEGER NULL,
    tas_movie_tree_id INTEGER NULL,
    status TEXT NOT NULL CHECK(status IN ('PENDING','ACTIVE','READY','FAILED','INTERRUPTED','CANCELED')),
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL,
    FOREIGN KEY(parent_route_node_id) REFERENCES atr_route_node(route_node_id)
);

CREATE UNIQUE INDEX ux_atr_route_root_source_dtm
    ON atr_route_node(source_dtm_artifact_id)
    WHERE parent_route_node_id IS NULL AND source_dtm_artifact_id IS NOT NULL;
CREATE UNIQUE INDEX ux_atr_route_root_source_state
    ON atr_route_node(source_savestate_id)
    WHERE parent_route_node_id IS NULL AND source_dtm_artifact_id IS NULL;
CREATE UNIQUE INDEX ux_atr_route_activity
    ON atr_route_node(parent_route_node_id,activity_kind,activity_key)
    WHERE node_kind='ACTIVITY';
CREATE INDEX ix_atr_route_parent ON atr_route_node(parent_route_node_id,route_node_id);

CREATE TABLE atr_battle_set_route (
    battle_set_id INTEGER PRIMARY KEY,
    route_node_id INTEGER NOT NULL,
    FOREIGN KEY(battle_set_id) REFERENCES ab_battle_set(battle_set_id),
    FOREIGN KEY(route_node_id) REFERENCES atr_route_node(route_node_id)
);
CREATE INDEX ix_atr_battle_set_route_node ON atr_battle_set_route(route_node_id,battle_set_id);

CREATE TABLE atr_victory_branch (
    victory_branch_id INTEGER PRIMARY KEY,
    source_route_node_id INTEGER NOT NULL,
    checkpoint_route_node_id INTEGER NOT NULL UNIQUE,
    selected_turn_job_id INTEGER NOT NULL UNIQUE,
    workflow_instance_id INTEGER NULL,
    battle_recording_id INTEGER NULL,
    tas_movie_tree_id INTEGER NULL,
    checkpoint_savestate_id INTEGER NULL,
    validation_request_id INTEGER NULL,
    status TEXT NOT NULL CHECK(status IN ('PENDING','WAITING_COMPLETION','RECORDING','VALIDATING','READY','FAILED','INTERRUPTED','CANCELED')),
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL,
    FOREIGN KEY(source_route_node_id) REFERENCES atr_route_node(route_node_id),
    FOREIGN KEY(checkpoint_route_node_id) REFERENCES atr_route_node(route_node_id),
    FOREIGN KEY(selected_turn_job_id) REFERENCES ab_turn_job(turn_job_id),
    FOREIGN KEY(battle_recording_id) REFERENCES ab_battle_recording(battle_recording_id)
);
CREATE INDEX ix_atr_victory_branch_source ON atr_victory_branch(source_route_node_id,victory_branch_id);
CREATE INDEX ix_atr_victory_branch_workflow ON atr_victory_branch(workflow_instance_id);

COMMIT;
