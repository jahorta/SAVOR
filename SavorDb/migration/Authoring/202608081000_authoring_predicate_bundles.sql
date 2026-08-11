BEGIN IMMEDIATE;

CREATE TABLE au_predicate_definition_v2 (
    predicate_definition_id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    description TEXT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE au_predicate_definition_revision_v2 (
    predicate_definition_revision_id INTEGER PRIMARY KEY,
    predicate_definition_id INTEGER NOT NULL,
    revision_number INTEGER NOT NULL CHECK(revision_number > 0),
    revision_state TEXT NOT NULL CHECK(revision_state IN ('DRAFT','PUBLISHED')),
    root_node_ordinal INTEGER NOT NULL CHECK(root_node_ordinal >= 0),
    content_sha256 TEXT NULL CHECK(content_sha256 IS NULL OR length(content_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    FOREIGN KEY(predicate_definition_id)
        REFERENCES au_predicate_definition_v2(predicate_definition_id),
    UNIQUE(predicate_definition_id, revision_number)
);

CREATE TABLE au_predicate_witness_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    witness_ordinal INTEGER NOT NULL CHECK(witness_ordinal >= 0),
    witness_name TEXT NOT NULL,
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    PRIMARY KEY(predicate_definition_revision_id, witness_ordinal),
    UNIQUE(predicate_definition_revision_id, witness_name),
    FOREIGN KEY(predicate_definition_revision_id)
        REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id)
        ON DELETE CASCADE,
    CHECK((value_builtin_type IS NOT NULL AND value_schema_canonical_id IS NULL
           AND value_schema_revision IS NULL AND value_schema_sha256 IS NULL)
       OR (value_builtin_type IS NULL AND value_schema_canonical_id IS NOT NULL
           AND value_schema_revision > 0 AND length(value_schema_sha256)=64))
);

CREATE TABLE au_predicate_expression_node_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    node_ordinal INTEGER NOT NULL CHECK(node_ordinal >= 0),
    node_kind TEXT NOT NULL CHECK(node_kind IN (
        'WITNESS','LITERAL','EQUAL','NOT_EQUAL','LESS','LESS_EQUAL',
        'GREATER','GREATER_EQUAL','BOOLEAN_AND','BOOLEAN_OR','BOOLEAN_NOT',
        'ADD','SUBTRACT','MULTIPLY','DIVIDE','REMAINDER','REGISTERED_REDUCER')),
    result_builtin_type INTEGER NULL,
    result_schema_canonical_id TEXT NULL,
    result_schema_revision INTEGER NULL,
    result_schema_sha256 TEXT NULL,
    witness_ordinal INTEGER NULL,
    literal_kind TEXT NULL CHECK(literal_kind IS NULL OR literal_kind IN
        ('BOOL','U8','U16','U32','U64','I32','I64','F32','F64','TEXT','BYTES','ENUM')),
    literal_integer INTEGER NULL,
    literal_real REAL NULL,
    literal_text TEXT NULL,
    literal_blob BLOB NULL,
    reducer_canonical_id TEXT NULL,
    reducer_revision INTEGER NULL,
    reducer_sha256 TEXT NULL,
    source_label TEXT NOT NULL,
    PRIMARY KEY(predicate_definition_revision_id, node_ordinal),
    FOREIGN KEY(predicate_definition_revision_id)
        REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id)
        ON DELETE CASCADE,
    FOREIGN KEY(predicate_definition_revision_id, witness_ordinal)
        REFERENCES au_predicate_witness_v2(predicate_definition_revision_id, witness_ordinal),
    CHECK((result_builtin_type IS NOT NULL AND result_schema_canonical_id IS NULL
           AND result_schema_revision IS NULL AND result_schema_sha256 IS NULL)
       OR (result_builtin_type IS NULL AND result_schema_canonical_id IS NOT NULL
           AND result_schema_revision > 0 AND length(result_schema_sha256)=64)),
    CHECK((node_kind='WITNESS' AND witness_ordinal IS NOT NULL)
       OR (node_kind<>'WITNESS' AND witness_ordinal IS NULL)),
    CHECK((node_kind='LITERAL' AND literal_kind IS NOT NULL)
       OR (node_kind<>'LITERAL' AND literal_kind IS NULL)),
    CHECK((node_kind='REGISTERED_REDUCER' AND reducer_canonical_id IS NOT NULL
           AND reducer_revision > 0 AND length(reducer_sha256)=64)
       OR (node_kind<>'REGISTERED_REDUCER' AND reducer_canonical_id IS NULL
           AND reducer_revision IS NULL AND reducer_sha256 IS NULL))
);

CREATE TABLE au_predicate_expression_edge_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    node_ordinal INTEGER NOT NULL,
    operand_ordinal INTEGER NOT NULL CHECK(operand_ordinal >= 0),
    operand_node_ordinal INTEGER NOT NULL,
    PRIMARY KEY(predicate_definition_revision_id, node_ordinal, operand_ordinal),
    FOREIGN KEY(predicate_definition_revision_id, node_ordinal)
        REFERENCES au_predicate_expression_node_v2(predicate_definition_revision_id, node_ordinal)
        ON DELETE CASCADE,
    FOREIGN KEY(predicate_definition_revision_id, operand_node_ordinal)
        REFERENCES au_predicate_expression_node_v2(predicate_definition_revision_id, node_ordinal),
    CHECK(operand_node_ordinal < node_ordinal)
);

CREATE TABLE au_predicate_bundle_v2 (
    predicate_bundle_id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    description TEXT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE au_predicate_bundle_revision_v2 (
    predicate_bundle_revision_id INTEGER PRIMARY KEY,
    predicate_bundle_id INTEGER NOT NULL,
    revision_number INTEGER NOT NULL CHECK(revision_number > 0),
    revision_state TEXT NOT NULL CHECK(revision_state IN ('DRAFT','PUBLISHED')),
    content_sha256 TEXT NULL CHECK(content_sha256 IS NULL OR length(content_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    FOREIGN KEY(predicate_bundle_id)
        REFERENCES au_predicate_bundle_v2(predicate_bundle_id),
    UNIQUE(predicate_bundle_id, revision_number)
);

CREATE TABLE au_predicate_bundle_parameter_v2 (
    predicate_bundle_revision_id INTEGER NOT NULL,
    parameter_ordinal INTEGER NOT NULL CHECK(parameter_ordinal >= 0),
    parameter_name TEXT NOT NULL,
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    PRIMARY KEY(predicate_bundle_revision_id, parameter_ordinal),
    UNIQUE(predicate_bundle_revision_id, parameter_name),
    FOREIGN KEY(predicate_bundle_revision_id)
        REFERENCES au_predicate_bundle_revision_v2(predicate_bundle_revision_id)
        ON DELETE CASCADE
);

CREATE TABLE au_predicate_observation_v2 (
    predicate_bundle_revision_id INTEGER NOT NULL,
    observation_ordinal INTEGER NOT NULL CHECK(observation_ordinal >= 0),
    stable_key TEXT NOT NULL,
    semantic_hook_id TEXT NOT NULL,
    source_kind TEXT NOT NULL CHECK(source_kind IN
        ('GUEST_ADDRESS','REGISTERED_QUERY','HOOK_RECEIPT','REGISTERED_REDUCER')),
    source_canonical_id TEXT NOT NULL,
    source_revision INTEGER NOT NULL CHECK(source_revision > 0),
    source_sha256 TEXT NOT NULL CHECK(length(source_sha256)=64),
    pinned_guest_address INTEGER NULL CHECK(pinned_guest_address IS NULL OR pinned_guest_address >= 0),
    source_field TEXT NULL,
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    PRIMARY KEY(predicate_bundle_revision_id, observation_ordinal),
    UNIQUE(predicate_bundle_revision_id, stable_key),
    FOREIGN KEY(predicate_bundle_revision_id)
        REFERENCES au_predicate_bundle_revision_v2(predicate_bundle_revision_id)
        ON DELETE CASCADE
);

CREATE TABLE au_predicate_baseline_v2 (
    predicate_bundle_revision_id INTEGER NOT NULL,
    baseline_ordinal INTEGER NOT NULL CHECK(baseline_ordinal >= 0),
    baseline_name TEXT NOT NULL,
    capture_hook_id TEXT NOT NULL,
    observation_ordinal INTEGER NOT NULL,
    update_policy TEXT NOT NULL CHECK(update_policy IN ('FIRST','LATEST')),
    PRIMARY KEY(predicate_bundle_revision_id, baseline_ordinal),
    UNIQUE(predicate_bundle_revision_id, baseline_name),
    FOREIGN KEY(predicate_bundle_revision_id, observation_ordinal)
        REFERENCES au_predicate_observation_v2(predicate_bundle_revision_id, observation_ordinal)
);

CREATE TABLE au_predicate_check_use_v2 (
    predicate_bundle_revision_id INTEGER NOT NULL,
    check_ordinal INTEGER NOT NULL CHECK(check_ordinal >= 0),
    stable_key TEXT NOT NULL,
    predicate_definition_revision_id INTEGER NOT NULL,
    semantic_hook_id TEXT NOT NULL,
    occurrence_policy TEXT NOT NULL CHECK(occurrence_policy IN
        ('FIRST','EVERY','ORDINAL','GUARD_ONCE')),
    occurrence_ordinal INTEGER NULL CHECK(occurrence_ordinal IS NULL OR occurrence_ordinal > 0),
    guard_predicate_definition_revision_id INTEGER NULL,
    reaction TEXT NOT NULL CHECK(reaction IN ('RECORD_AND_CONTINUE','ABORT_ON_FAIL')),
    participates_in_aggregation INTEGER NOT NULL CHECK(participates_in_aggregation IN (0,1)),
    emit_evidence INTEGER NOT NULL CHECK(emit_evidence IN (0,1)),
    PRIMARY KEY(predicate_bundle_revision_id, check_ordinal),
    UNIQUE(predicate_bundle_revision_id, stable_key),
    FOREIGN KEY(predicate_bundle_revision_id)
        REFERENCES au_predicate_bundle_revision_v2(predicate_bundle_revision_id)
        ON DELETE CASCADE,
    FOREIGN KEY(predicate_definition_revision_id)
        REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id),
    FOREIGN KEY(guard_predicate_definition_revision_id)
        REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id),
    CHECK((occurrence_policy='ORDINAL' AND occurrence_ordinal IS NOT NULL)
       OR (occurrence_policy<>'ORDINAL' AND occurrence_ordinal IS NULL)),
    CHECK((occurrence_policy='GUARD_ONCE' AND guard_predicate_definition_revision_id IS NOT NULL)
       OR (occurrence_policy<>'GUARD_ONCE' AND guard_predicate_definition_revision_id IS NULL))
);

CREATE TABLE au_predicate_witness_binding_v2 (
    predicate_bundle_revision_id INTEGER NOT NULL,
    check_ordinal INTEGER NOT NULL,
    witness_ordinal INTEGER NOT NULL,
    source_kind TEXT NOT NULL CHECK(source_kind IN
        ('OBSERVATION','BASELINE','PARAMETER','HOOK_RECEIPT','LITERAL')),
    source_ordinal INTEGER NULL,
    source_field TEXT NULL,
    literal_kind TEXT NULL,
    literal_integer INTEGER NULL,
    literal_real REAL NULL,
    literal_text TEXT NULL,
    literal_blob BLOB NULL,
    PRIMARY KEY(predicate_bundle_revision_id, check_ordinal, witness_ordinal),
    FOREIGN KEY(predicate_bundle_revision_id, check_ordinal)
        REFERENCES au_predicate_check_use_v2(predicate_bundle_revision_id, check_ordinal)
        ON DELETE CASCADE
);

-- The zero-check published bundle is the canonical execution default.
INSERT INTO au_predicate_bundle_v2(
    predicate_bundle_id,stable_key,name,description,created_at_utc)
VALUES(1,'savor.predicate_bundle.empty','Empty predicate bundle',
       'Canonical bundle containing no checks',0);
INSERT INTO au_predicate_bundle_revision_v2(
    predicate_bundle_revision_id,predicate_bundle_id,revision_number,
    revision_state,content_sha256,created_at_utc,published_at_utc)
VALUES(1,1,1,'PUBLISHED',
       'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',0,0);

ALTER TABLE au_battle_plan_turn
    ADD COLUMN default_predicate_bundle_revision_id INTEGER NULL
    REFERENCES au_predicate_bundle_revision_v2(predicate_bundle_revision_id);

CREATE TRIGGER au_predicate_definition_published_immutable
BEFORE UPDATE ON au_predicate_definition_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN
    SELECT RAISE(ABORT,'published predicate definition revisions are immutable');
END;

CREATE TRIGGER au_predicate_bundle_published_immutable
BEFORE UPDATE ON au_predicate_bundle_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN
    SELECT RAISE(ABORT,'published predicate bundle revisions are immutable');
END;

CREATE TRIGGER au_predicate_definition_child_immutable
BEFORE INSERT ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2
      WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN
    SELECT RAISE(ABORT,'published predicate definition revisions are immutable');
END;

CREATE TRIGGER au_predicate_bundle_child_immutable
BEFORE INSERT ON au_predicate_check_use_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2
      WHERE predicate_bundle_revision_id=NEW.predicate_bundle_revision_id)='PUBLISHED'
BEGIN
    SELECT RAISE(ABORT,'published predicate bundle revisions are immutable');
END;

CREATE TRIGGER au_predicate_definition_published_delete_immutable
BEFORE DELETE ON au_predicate_definition_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;

CREATE TRIGGER au_predicate_bundle_published_delete_immutable
BEFORE DELETE ON au_predicate_bundle_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

CREATE TRIGGER au_predicate_witness_published_insert_immutable
BEFORE INSERT ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_witness_published_update_immutable
BEFORE UPDATE ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_witness_published_delete_immutable
BEFORE DELETE ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;

CREATE TRIGGER au_predicate_node_published_update_immutable
BEFORE UPDATE ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_node_published_delete_immutable
BEFORE DELETE ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;

CREATE TRIGGER au_predicate_edge_published_insert_immutable
BEFORE INSERT ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_edge_published_update_immutable
BEFORE UPDATE ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_edge_published_delete_immutable
BEFORE DELETE ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;

CREATE TRIGGER au_predicate_parameter_published_insert_immutable BEFORE INSERT ON au_predicate_bundle_parameter_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=NEW.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_parameter_published_update_immutable BEFORE UPDATE ON au_predicate_bundle_parameter_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_parameter_published_delete_immutable BEFORE DELETE ON au_predicate_bundle_parameter_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

CREATE TRIGGER au_predicate_observation_published_insert_immutable BEFORE INSERT ON au_predicate_observation_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=NEW.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_observation_published_update_immutable BEFORE UPDATE ON au_predicate_observation_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_observation_published_delete_immutable BEFORE DELETE ON au_predicate_observation_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

CREATE TRIGGER au_predicate_baseline_published_insert_immutable BEFORE INSERT ON au_predicate_baseline_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=NEW.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_baseline_published_update_immutable BEFORE UPDATE ON au_predicate_baseline_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_baseline_published_delete_immutable BEFORE DELETE ON au_predicate_baseline_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

CREATE TRIGGER au_predicate_check_published_update_immutable BEFORE UPDATE ON au_predicate_check_use_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_check_published_delete_immutable BEFORE DELETE ON au_predicate_check_use_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

CREATE TRIGGER au_predicate_binding_published_insert_immutable BEFORE INSERT ON au_predicate_witness_binding_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=NEW.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_binding_published_update_immutable BEFORE UPDATE ON au_predicate_witness_binding_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;
CREATE TRIGGER au_predicate_binding_published_delete_immutable BEFORE DELETE ON au_predicate_witness_binding_v2
WHEN (SELECT revision_state FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_revision_id=OLD.predicate_bundle_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate bundle revisions are immutable'); END;

COMMIT;
