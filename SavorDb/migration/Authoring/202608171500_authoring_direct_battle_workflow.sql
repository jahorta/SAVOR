PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

-- Choice is a saved workflow-contract type.  Its ordered token catalog is
-- immutable with the graph revision and is never inferred by a launcher.
ALTER TABLE au_workflow_graph_revision_node_argument
    RENAME TO au_workflow_graph_revision_node_argument_legacy;

CREATE TABLE au_workflow_graph_revision_node_argument (
    workflow_graph_revision_node_argument_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_id INTEGER NOT NULL,
    argument_key TEXT NOT NULL,
    display_name TEXT NOT NULL,
    value_type TEXT NOT NULL CHECK(value_type IN ('integer','text','boolean','json','choice')),
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    default_value TEXT NULL,
    minimum_integer INTEGER NULL,
    maximum_integer INTEGER NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(workflow_graph_revision_node_id) REFERENCES au_workflow_graph_revision_node(workflow_graph_revision_node_id),
    CONSTRAINT uq_au_workflow_graph_revision_node_argument UNIQUE (workflow_graph_revision_node_id, argument_key),
    CHECK((value_type='integer') OR (minimum_integer IS NULL AND maximum_integer IS NULL))
);

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_argument_id,workflow_graph_revision_node_id,
    argument_key,display_name,value_type,required,default_value,
    minimum_integer,maximum_integer,ordinal)
SELECT workflow_graph_revision_node_argument_id,workflow_graph_revision_node_id,
       argument_key,display_name,value_type,required,default_value,
       minimum_integer,maximum_integer,ordinal
FROM au_workflow_graph_revision_node_argument_legacy;

DROP TABLE au_workflow_graph_revision_node_argument_legacy;

CREATE TABLE au_workflow_graph_revision_node_argument_choice (
    workflow_graph_revision_node_argument_choice_id INTEGER PRIMARY KEY,
    workflow_graph_revision_node_argument_id INTEGER NOT NULL,
    choice_value TEXT NOT NULL CHECK(length(choice_value)>0),
    display_name TEXT NOT NULL CHECK(length(display_name)>0),
    ordinal INTEGER NOT NULL CHECK(ordinal>=0),
    FOREIGN KEY(workflow_graph_revision_node_argument_id)
        REFERENCES au_workflow_graph_revision_node_argument(workflow_graph_revision_node_argument_id),
    CONSTRAINT uq_au_workflow_graph_argument_choice_ordinal UNIQUE(
        workflow_graph_revision_node_argument_id,ordinal),
    CONSTRAINT uq_au_workflow_graph_argument_choice_value UNIQUE(
        workflow_graph_revision_node_argument_id,choice_value)
);

-- Saved Battle graphs retain topology but deliberately lose the obsolete
-- wrapper reference.  The user must select one exact Battle Plan again.
UPDATE au_workflow_graph_revision
SET graph_hash='direct-battle-plan-v1:' || graph_hash
WHERE workflow_graph_revision_id IN (
    SELECT workflow_graph_revision_id
    FROM au_workflow_graph_revision_node
    WHERE unit_kind='battle_chain'
);

UPDATE au_workflow_graph_revision_node
SET unit_kind='battle',
    display_name='Battle',
    authored_ref_kind='authoring.battle_plan',
    authored_ref_id=NULL
WHERE unit_kind='battle_chain';

DELETE FROM au_workflow_graph_revision_node_argument_constraint
WHERE workflow_graph_revision_node_id IN (
    SELECT workflow_graph_revision_node_id
    FROM au_workflow_graph_revision_node
    WHERE unit_kind='battle'
);

DELETE FROM au_workflow_graph_revision_node_argument
WHERE workflow_graph_revision_node_id IN (
    SELECT workflow_graph_revision_node_id
    FROM au_workflow_graph_revision_node
    WHERE unit_kind='battle'
);

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id,argument_key,display_name,value_type,
    required,default_value,minimum_integer,maximum_integer,ordinal)
SELECT workflow_graph_revision_node_id,'continuation_mode','Continuation','choice',
       1,NULL,NULL,NULL,0
FROM au_workflow_graph_revision_node WHERE unit_kind='battle';

INSERT INTO au_workflow_graph_revision_node_argument_choice(
    workflow_graph_revision_node_argument_id,choice_value,display_name,ordinal)
SELECT workflow_graph_revision_node_argument_id,'manual_selection',
       'Manual selection after each wave',0
FROM au_workflow_graph_revision_node_argument
WHERE argument_key='continuation_mode';

INSERT INTO au_workflow_graph_revision_node_argument_choice(
    workflow_graph_revision_node_argument_id,choice_value,display_name,ordinal)
SELECT workflow_graph_revision_node_argument_id,'automatic_best_per_ending_rng',
       'Automatically continue the best candidate for each ending RNG',1
FROM au_workflow_graph_revision_node_argument
WHERE argument_key='continuation_mode';

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id,argument_key,display_name,value_type,
    required,default_value,minimum_integer,maximum_integer,ordinal)
SELECT workflow_graph_revision_node_id,'fake_attack_min','Minimum fake attacks','integer',
       0,'0',0,2147483647,1
FROM au_workflow_graph_revision_node WHERE unit_kind='battle';

INSERT INTO au_workflow_graph_revision_node_argument(
    workflow_graph_revision_node_id,argument_key,display_name,value_type,
    required,default_value,minimum_integer,maximum_integer,ordinal)
SELECT workflow_graph_revision_node_id,'fake_attack_max','Maximum fake attacks','integer',
       0,'0',0,2147483647,2
FROM au_workflow_graph_revision_node WHERE unit_kind='battle';

INSERT INTO au_workflow_graph_revision_node_argument_constraint(
    workflow_graph_revision_node_id,lesser_or_equal_key,greater_or_equal_key,
    message,ordinal)
SELECT workflow_graph_revision_node_id,'fake_attack_min','fake_attack_max',
       'minimum fake attacks must not exceed maximum fake attacks',0
FROM au_workflow_graph_revision_node WHERE unit_kind='battle';

-- Unreleased wrapper objects are a hard cut, not migration inputs.
DROP TABLE IF EXISTS au_battle_chain_spec;
DROP TABLE IF EXISTS au_explorer_settings;
DROP TABLE IF EXISTS au_battle_run_spec;

COMMIT;
PRAGMA foreign_keys = ON;
