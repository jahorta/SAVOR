PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

CREATE TEMP TABLE _au_workflow_contract_guard(
    unknown_count INTEGER NOT NULL CHECK(unknown_count = 0)
);
INSERT INTO _au_workflow_contract_guard(unknown_count)
SELECT COUNT(1)
FROM (
    SELECT data_kind FROM au_workflow_graph_revision_node_input
    UNION ALL
    SELECT data_kind FROM au_workflow_graph_revision_node_output
)
WHERE data_kind NOT IN (
    'state_artifact.dtm_artifact_id',
    'state.savestate_id',
    'state.movie_active_savestate_id',
    'state.movie_inactive_savestate_id',
    'state.movie_paired_savestate_id',
    'analysis.tas_movie_validation_attempt_id',
    'state.tas_movie_tree_id',
    'analysis.seed_probe_run',
    'analysis.input_frame_set_id',
    'analysis_battle.battle_context_id',
    'analysis_battle.battle_set',
    'analysis.battle_manual_followup_id',
    'analysis_battle.battle_turn_job',
    'analysis_battle.battle_completion',
    'analysis_battle.battle_recording',
    'analysis_battle.battle_replay',
    'state_artifact.navigation_context_id'
);
DROP TABLE _au_workflow_contract_guard;

ALTER TABLE au_workflow_graph_revision_node_input
    RENAME TO au_workflow_graph_revision_node_input_legacy;
CREATE TABLE au_workflow_graph_revision_node_input (
    workflow_graph_revision_node_input_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    input_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL CHECK(length(ref_kind) > 0),
    display_name TEXT NULL,
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_input UNIQUE (workflow_graph_revision_node_id, input_key)
);
INSERT INTO au_workflow_graph_revision_node_input(
    workflow_graph_revision_node_input_id, workflow_graph_revision_node_id,
    input_key, data_kind, ref_kind, display_name, required, ordinal)
SELECT workflow_graph_revision_node_input_id, workflow_graph_revision_node_id,
       input_key, data_kind,
       CASE
           WHEN data_kind = 'state_artifact.dtm_artifact_id' THEN 'state_artifact'
           WHEN data_kind IN ('state.savestate_id','state.movie_active_savestate_id','state.movie_inactive_savestate_id','state.movie_paired_savestate_id') THEN 'state.savestate'
           WHEN data_kind = 'analysis.tas_movie_validation_attempt_id' THEN 'tmv_validation_attempt'
           WHEN data_kind = 'state.tas_movie_tree_id' THEN 'state_tas_movie_tree'
           WHEN data_kind = 'analysis.seed_probe_run' THEN 'sp_probe_run'
           WHEN data_kind = 'analysis.input_frame_set_id' AND input_key = 'initial_input_frames' THEN 'au.input_set'
           WHEN data_kind = 'analysis.input_frame_set_id' THEN 'an.input_set'
           WHEN data_kind = 'analysis_battle.battle_context_id' THEN 'ab_battle_context'
           WHEN data_kind = 'analysis_battle.battle_set' THEN 'analysis_battle.battle_set'
           WHEN data_kind = 'analysis.battle_manual_followup_id' THEN 'manual_followup'
           WHEN data_kind = 'analysis_battle.battle_turn_job' THEN 'analysis_battle.turn_job'
           WHEN data_kind = 'analysis_battle.battle_completion' THEN 'analysis_battle.battle_completion'
           WHEN data_kind = 'analysis_battle.battle_recording' THEN 'analysis_battle.battle_recording'
           WHEN data_kind = 'analysis_battle.battle_replay' THEN 'analysis_battle.battle_replay'
           WHEN data_kind = 'state_artifact.navigation_context_id' THEN 'state_artifact'
       END,
       display_name, required, ordinal
FROM au_workflow_graph_revision_node_input_legacy;
DROP TABLE au_workflow_graph_revision_node_input_legacy;

ALTER TABLE au_workflow_graph_revision_node_output
    RENAME TO au_workflow_graph_revision_node_output_legacy;
CREATE TABLE au_workflow_graph_revision_node_output (
    workflow_graph_revision_node_output_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    output_key TEXT NOT NULL,
    data_kind TEXT NOT NULL,
    ref_kind TEXT NOT NULL CHECK(length(ref_kind) > 0),
    display_name TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_output UNIQUE (workflow_graph_revision_node_id, output_key)
);
INSERT INTO au_workflow_graph_revision_node_output(
    workflow_graph_revision_node_output_id, workflow_graph_revision_node_id,
    output_key, data_kind, ref_kind, display_name, ordinal)
SELECT workflow_graph_revision_node_output_id, workflow_graph_revision_node_id,
       output_key, data_kind,
       CASE
           WHEN data_kind = 'state_artifact.dtm_artifact_id' THEN 'state_artifact'
           WHEN data_kind IN ('state.savestate_id','state.movie_active_savestate_id','state.movie_inactive_savestate_id','state.movie_paired_savestate_id') THEN 'state.savestate'
           WHEN data_kind = 'analysis.tas_movie_validation_attempt_id' THEN 'tmv_validation_attempt'
           WHEN data_kind = 'state.tas_movie_tree_id' THEN 'state_tas_movie_tree'
           WHEN data_kind = 'analysis.seed_probe_run' THEN 'sp_probe_run'
           WHEN data_kind = 'analysis.input_frame_set_id' THEN 'an.input_set'
           WHEN data_kind = 'analysis_battle.battle_context_id' THEN 'ab_battle_context'
           WHEN data_kind = 'analysis_battle.battle_set' THEN 'analysis_battle.battle_set'
           WHEN data_kind = 'analysis.battle_manual_followup_id' THEN 'manual_followup'
           WHEN data_kind = 'analysis_battle.battle_turn_job' THEN 'analysis_battle.turn_job'
           WHEN data_kind = 'analysis_battle.battle_completion' THEN 'analysis_battle.battle_completion'
           WHEN data_kind = 'analysis_battle.battle_recording' THEN 'analysis_battle.battle_recording'
           WHEN data_kind = 'analysis_battle.battle_replay' THEN 'analysis_battle.battle_replay'
           WHEN data_kind = 'state_artifact.navigation_context_id' THEN 'state_artifact'
       END,
       display_name, ordinal
FROM au_workflow_graph_revision_node_output_legacy;
DROP TABLE au_workflow_graph_revision_node_output_legacy;

CREATE TABLE au_workflow_graph_revision_node_argument (
    workflow_graph_revision_node_argument_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    argument_key TEXT NOT NULL,
    display_name TEXT NOT NULL,
    value_type TEXT NOT NULL CHECK(value_type IN ('integer','text','boolean','json')),
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    default_value TEXT NULL,
    minimum_integer INTEGER NULL,
    maximum_integer INTEGER NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_argument UNIQUE (workflow_graph_revision_node_id, argument_key)
);

CREATE TABLE au_workflow_graph_revision_node_argument_constraint (
    workflow_graph_revision_node_argument_constraint_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    lesser_or_equal_key TEXT NOT NULL,
    greater_or_equal_key TEXT NOT NULL,
    message TEXT NOT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_argument_constraint UNIQUE (
        workflow_graph_revision_node_id, lesser_or_equal_key, greater_or_equal_key)
);

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id, argument_key, display_name, value_type,
    required, default_value, minimum_integer, maximum_integer, ordinal)
SELECT workflow_graph_revision_node_id, 'rtc', 'RTC', 'integer', 1, NULL, 0, 4294967295, 0
FROM au_workflow_graph_revision_node WHERE unit_kind = 'tas_movie_validate_root';

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id, argument_key, display_name, value_type,
    required, default_value, minimum_integer, maximum_integer, ordinal)
SELECT workflow_graph_revision_node_id, 'samples_per_axis', 'Samples per axis', 'integer', 0, '5', 1, 64, 0
FROM au_workflow_graph_revision_node
WHERE unit_kind IN ('seed_probe_chain','battle_seed_probe','dungeon_seed_probe','overworld_seed_probe');

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id, argument_key, display_name, value_type,
    required, default_value, minimum_integer, maximum_integer, ordinal)
SELECT workflow_graph_revision_node_id, 'fake_attack_min', 'Minimum fake attacks', 'integer', 0, '0', 0, 2147483647, 0
FROM au_workflow_graph_revision_node WHERE unit_kind = 'battle_chain';
INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id, argument_key, display_name, value_type,
    required, default_value, minimum_integer, maximum_integer, ordinal)
SELECT workflow_graph_revision_node_id, 'fake_attack_max', 'Maximum fake attacks', 'integer', 0, '0', 0, 2147483647, 1
FROM au_workflow_graph_revision_node WHERE unit_kind = 'battle_chain';
INSERT INTO au_workflow_graph_revision_node_argument_constraint(
    workflow_graph_revision_node_id, lesser_or_equal_key, greater_or_equal_key,
    message, ordinal)
SELECT workflow_graph_revision_node_id, 'fake_attack_min', 'fake_attack_max',
       'minimum fake attacks must not exceed maximum fake attacks', 0
FROM au_workflow_graph_revision_node WHERE unit_kind = 'battle_chain';

UPDATE au_battle_plan_turn
SET default_predicate_bundle_revision_id = NULL
WHERE default_predicate_bundle_revision_id = 1;

COMMIT;
PRAGMA foreign_keys = ON;
