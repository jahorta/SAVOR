BEGIN IMMEDIATE;

-- Forward-only repair for databases that recorded the unreleased 202608161400
-- hard cut while still containing an earlier physical predicate schema.
-- Predicate authoring content is intentionally discarded; all other Authoring
-- data, including Battle Plans, remains intact.
UPDATE au_battle_plan_turn SET default_predicate_group_revision_id=NULL;

DROP TRIGGER IF EXISTS au_predicate_definition_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_immutable;
DROP TRIGGER IF EXISTS au_predicate_witness_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_witness_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_witness_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_node_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_node_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_node_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_edge_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_edge_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_edge_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_bundle_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_bundle_child_immutable;
DROP TRIGGER IF EXISTS au_predicate_bundle_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_parameter_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_parameter_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_parameter_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_observation_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_observation_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_observation_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_baseline_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_baseline_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_baseline_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_check_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_check_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_check_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_binding_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_binding_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_binding_published_delete_immutable;

DROP TABLE IF EXISTS au_predicate_group_member_hook;
DROP TABLE IF EXISTS au_predicate_group_member;
DROP TABLE IF EXISTS au_predicate_group_revision;
DROP TABLE IF EXISTS au_predicate_group;
DROP TABLE IF EXISTS au_predicate_execution_binding_witness_source;
DROP TABLE IF EXISTS au_predicate_execution_binding_revision;
DROP TABLE IF EXISTS au_predicate_execution_binding;
DROP TABLE IF EXISTS au_predicate_expression_edge_v2;
DROP TABLE IF EXISTS au_predicate_expression_node_v2;
DROP TABLE IF EXISTS au_predicate_witness_v2;
DROP TABLE IF EXISTS au_predicate_definition_revision_v2;
DROP TABLE IF EXISTS au_predicate_definition_v2;
DROP TABLE IF EXISTS au_predicate_authoring_request;
DROP TABLE IF EXISTS au_predicate_witness_binding_v2;
DROP TABLE IF EXISTS au_predicate_check_use_v2;
DROP TABLE IF EXISTS au_predicate_baseline_v2;
DROP TABLE IF EXISTS au_predicate_observation_v2;
DROP TABLE IF EXISTS au_predicate_bundle_parameter_v2;
DROP TABLE IF EXISTS au_predicate_bundle_revision_v2;
DROP TABLE IF EXISTS au_predicate_bundle_v2;

CREATE TABLE au_predicate_authoring_request (
    operation_kind TEXT NOT NULL,
    creation_request_key TEXT NOT NULL CHECK(
        length(creation_request_key)=32 AND creation_request_key=lower(creation_request_key) AND
        creation_request_key NOT GLOB '*[^0-9a-f]*'),
    request_sha256 TEXT NOT NULL CHECK(length(request_sha256)=64),
    parent_id INTEGER NOT NULL,
    revision_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    PRIMARY KEY(operation_kind,creation_request_key)
);

CREATE TABLE au_predicate_definition_v2 (
    predicate_definition_id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE CHECK(
        substr(stable_key,1,21)='predicate.definition/' AND length(stable_key)=53 AND
        substr(stable_key,22) NOT GLOB '*[^0-9a-f]*'),
    name TEXT NOT NULL CHECK(length(name)>0),
    description TEXT NOT NULL DEFAULT '',
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

CREATE TABLE au_predicate_definition_revision_v2 (
    predicate_definition_revision_id INTEGER PRIMARY KEY,
    predicate_definition_id INTEGER NOT NULL,
    revision_number INTEGER NOT NULL CHECK(revision_number>0),
    revision_state TEXT NOT NULL CHECK(revision_state IN ('DRAFT','PUBLISHED')),
    root_node_ordinal INTEGER NOT NULL CHECK(root_node_ordinal>=0),
    semantic_sha256 TEXT NOT NULL CHECK(length(semantic_sha256)=64),
    content_sha256 TEXT NULL CHECK(content_sha256 IS NULL OR length(content_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    FOREIGN KEY(predicate_definition_id) REFERENCES au_predicate_definition_v2(predicate_definition_id),
    UNIQUE(predicate_definition_id,revision_number),
    UNIQUE(predicate_definition_id,semantic_sha256)
);

CREATE UNIQUE INDEX uq_au_predicate_definition_one_draft
ON au_predicate_definition_revision_v2(predicate_definition_id)
WHERE revision_state='DRAFT';

CREATE TABLE au_predicate_witness_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    witness_ordinal INTEGER NOT NULL CHECK(witness_ordinal>=0),
    witness_name TEXT NOT NULL,
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    PRIMARY KEY(predicate_definition_revision_id,witness_ordinal),
    UNIQUE(predicate_definition_revision_id,witness_name),
    FOREIGN KEY(predicate_definition_revision_id) REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id) ON DELETE CASCADE,
    CHECK((value_builtin_type IS NOT NULL AND value_schema_canonical_id IS NULL AND value_schema_revision IS NULL AND value_schema_sha256 IS NULL)
       OR (value_builtin_type IS NULL AND value_schema_canonical_id IS NOT NULL AND value_schema_revision > 0 AND length(value_schema_sha256)=64))
);

CREATE TABLE au_predicate_expression_node_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    node_ordinal INTEGER NOT NULL CHECK(node_ordinal>=0),
    node_kind TEXT NOT NULL CHECK(node_kind IN (
        'WITNESS','LITERAL','EQUAL','NOT_EQUAL','LESS','LESS_EQUAL','GREATER','GREATER_EQUAL',
        'BOOLEAN_AND','BOOLEAN_OR','BOOLEAN_NOT','ADD','SUBTRACT','MULTIPLY','DIVIDE','REMAINDER','REGISTERED_REDUCER')),
    result_builtin_type INTEGER NULL,
    result_schema_canonical_id TEXT NULL,
    result_schema_revision INTEGER NULL,
    result_schema_sha256 TEXT NULL,
    witness_ordinal INTEGER NULL,
    literal_kind TEXT NULL CHECK(literal_kind IS NULL OR literal_kind IN ('BOOL','U8','U16','U32','U64','I32','I64','F32','F64','TEXT','BYTES','ENUM')),
    literal_integer INTEGER NULL,
    literal_real REAL NULL,
    literal_text TEXT NULL,
    literal_blob BLOB NULL,
    reducer_canonical_id TEXT NULL,
    reducer_revision INTEGER NULL,
    reducer_sha256 TEXT NULL,
    source_label TEXT NOT NULL,
    PRIMARY KEY(predicate_definition_revision_id,node_ordinal),
    FOREIGN KEY(predicate_definition_revision_id) REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id) ON DELETE CASCADE,
    FOREIGN KEY(predicate_definition_revision_id,witness_ordinal) REFERENCES au_predicate_witness_v2(predicate_definition_revision_id,witness_ordinal),
    CHECK((result_builtin_type IS NOT NULL AND result_schema_canonical_id IS NULL AND result_schema_revision IS NULL AND result_schema_sha256 IS NULL)
       OR (result_builtin_type IS NULL AND result_schema_canonical_id IS NOT NULL AND result_schema_revision > 0 AND length(result_schema_sha256)=64)),
    CHECK((node_kind='WITNESS' AND witness_ordinal IS NOT NULL) OR (node_kind<>'WITNESS' AND witness_ordinal IS NULL)),
    CHECK((node_kind='LITERAL' AND literal_kind IS NOT NULL) OR (node_kind<>'LITERAL' AND literal_kind IS NULL)),
    CHECK((node_kind='REGISTERED_REDUCER' AND reducer_canonical_id IS NOT NULL AND reducer_revision > 0 AND length(reducer_sha256)=64)
       OR (node_kind<>'REGISTERED_REDUCER' AND reducer_canonical_id IS NULL AND reducer_revision IS NULL AND reducer_sha256 IS NULL))
);

CREATE TABLE au_predicate_expression_edge_v2 (
    predicate_definition_revision_id INTEGER NOT NULL,
    node_ordinal INTEGER NOT NULL,
    operand_ordinal INTEGER NOT NULL CHECK(operand_ordinal>=0),
    operand_node_ordinal INTEGER NOT NULL,
    PRIMARY KEY(predicate_definition_revision_id,node_ordinal,operand_ordinal),
    FOREIGN KEY(predicate_definition_revision_id,node_ordinal) REFERENCES au_predicate_expression_node_v2(predicate_definition_revision_id,node_ordinal) ON DELETE CASCADE,
    FOREIGN KEY(predicate_definition_revision_id,operand_node_ordinal) REFERENCES au_predicate_expression_node_v2(predicate_definition_revision_id,node_ordinal),
    CHECK(operand_node_ordinal<node_ordinal)
);

CREATE TABLE au_predicate_execution_binding (
    predicate_execution_binding_id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE CHECK(
        substr(stable_key,1,18)='predicate.binding/' AND length(stable_key)=50 AND
        substr(stable_key,19) NOT GLOB '*[^0-9a-f]*'),
    name TEXT NOT NULL CHECK(length(name)>0),
    description TEXT NOT NULL DEFAULT '',
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

CREATE TABLE au_predicate_execution_binding_revision (
    predicate_execution_binding_revision_id INTEGER PRIMARY KEY,
    predicate_execution_binding_id INTEGER NOT NULL,
    revision_number INTEGER NOT NULL CHECK(revision_number > 0),
    revision_state TEXT NOT NULL CHECK(revision_state IN ('DRAFT','PUBLISHED')),
    predicate_definition_revision_id INTEGER NOT NULL,
    predicate_definition_sha256 TEXT NOT NULL CHECK(length(predicate_definition_sha256)=64),
    semantic_sha256 TEXT NOT NULL CHECK(length(semantic_sha256)=64),
    content_sha256 TEXT NULL CHECK(content_sha256 IS NULL OR length(content_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    FOREIGN KEY(predicate_execution_binding_id) REFERENCES au_predicate_execution_binding(predicate_execution_binding_id),
    FOREIGN KEY(predicate_definition_revision_id) REFERENCES au_predicate_definition_revision_v2(predicate_definition_revision_id),
    UNIQUE(predicate_execution_binding_id,revision_number),
    UNIQUE(predicate_execution_binding_id,semantic_sha256)
);

CREATE UNIQUE INDEX uq_au_predicate_execution_binding_one_draft
ON au_predicate_execution_binding_revision(predicate_execution_binding_id)
WHERE revision_state='DRAFT';

CREATE TABLE au_predicate_execution_binding_witness_source (
    predicate_execution_binding_revision_id INTEGER NOT NULL,
    witness_ordinal INTEGER NOT NULL CHECK(witness_ordinal >= 0),
    source_kind TEXT NOT NULL CHECK(source_kind IN ('CONCRETE_VALUE','CURRENT_HOOK_QUERY','CURRENT_HOOK_RECEIPT','PINNED_GUEST_MEMORY','BASELINE_OBSERVATION')),
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    literal_kind TEXT NULL,
    literal_integer INTEGER NULL,
    literal_real REAL NULL,
    literal_text TEXT NULL,
    literal_blob BLOB NULL,
    observation_source_kind TEXT NULL CHECK(observation_source_kind IS NULL OR observation_source_kind IN ('GUEST_ADDRESS','REGISTERED_QUERY','HOOK_RECEIPT','REGISTERED_REDUCER')),
    source_canonical_id TEXT NULL,
    source_revision INTEGER NULL,
    source_sha256 TEXT NULL,
    source_field TEXT NULL,
    pinned_guest_address INTEGER NULL,
    baseline_capture_hook_id TEXT NULL,
    baseline_update_policy TEXT NULL CHECK(baseline_update_policy IS NULL OR baseline_update_policy IN ('FIRST','LATEST')),
    PRIMARY KEY(predicate_execution_binding_revision_id,witness_ordinal),
    FOREIGN KEY(predicate_execution_binding_revision_id) REFERENCES au_predicate_execution_binding_revision(predicate_execution_binding_revision_id) ON DELETE CASCADE
);

CREATE TABLE au_predicate_group (
    predicate_group_id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE CHECK(
        substr(stable_key,1,16)='predicate.group/' AND length(stable_key)=48 AND
        substr(stable_key,17) NOT GLOB '*[^0-9a-f]*'),
    name TEXT NOT NULL CHECK(length(name)>0),
    description TEXT NOT NULL DEFAULT '',
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

CREATE TABLE au_predicate_group_revision (
    predicate_group_revision_id INTEGER PRIMARY KEY,
    predicate_group_id INTEGER NOT NULL,
    revision_number INTEGER NOT NULL CHECK(revision_number > 0),
    revision_state TEXT NOT NULL CHECK(revision_state IN ('DRAFT','PUBLISHED')),
    semantic_sha256 TEXT NOT NULL CHECK(length(semantic_sha256)=64),
    content_sha256 TEXT NULL CHECK(content_sha256 IS NULL OR length(content_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    published_at_utc INTEGER NULL,
    FOREIGN KEY(predicate_group_id) REFERENCES au_predicate_group(predicate_group_id),
    UNIQUE(predicate_group_id,revision_number),
    UNIQUE(predicate_group_id,semantic_sha256)
);

CREATE UNIQUE INDEX uq_au_predicate_group_one_draft
ON au_predicate_group_revision(predicate_group_id)
WHERE revision_state='DRAFT';

CREATE TABLE au_predicate_group_member (
    predicate_group_revision_id INTEGER NOT NULL,
    member_ordinal INTEGER NOT NULL CHECK(member_ordinal >= 0),
    predicate_execution_binding_revision_id INTEGER NOT NULL,
    occurrence_policy TEXT NOT NULL CHECK(occurrence_policy IN ('FIRST','EVERY','ORDINAL','GUARD_ONCE')),
    occurrence_ordinal INTEGER NULL CHECK(occurrence_ordinal IS NULL OR occurrence_ordinal > 0),
    guard_execution_binding_revision_id INTEGER NULL,
    reaction TEXT NOT NULL CHECK(reaction IN ('RECORD_AND_CONTINUE','ABORT_ON_FAIL')),
    participates_in_aggregation INTEGER NOT NULL CHECK(participates_in_aggregation IN (0,1)),
    emit_evidence INTEGER NOT NULL CHECK(emit_evidence IN (0,1)),
    PRIMARY KEY(predicate_group_revision_id,member_ordinal),
    UNIQUE(predicate_group_revision_id,predicate_execution_binding_revision_id),
    FOREIGN KEY(predicate_group_revision_id) REFERENCES au_predicate_group_revision(predicate_group_revision_id) ON DELETE CASCADE,
    FOREIGN KEY(predicate_execution_binding_revision_id) REFERENCES au_predicate_execution_binding_revision(predicate_execution_binding_revision_id),
    FOREIGN KEY(guard_execution_binding_revision_id) REFERENCES au_predicate_execution_binding_revision(predicate_execution_binding_revision_id)
);

CREATE TABLE au_predicate_group_member_hook (
    predicate_group_revision_id INTEGER NOT NULL,
    member_ordinal INTEGER NOT NULL,
    semantic_hook_id TEXT NOT NULL,
    PRIMARY KEY(predicate_group_revision_id,member_ordinal,semantic_hook_id),
    FOREIGN KEY(predicate_group_revision_id,member_ordinal) REFERENCES au_predicate_group_member(predicate_group_revision_id,member_ordinal) ON DELETE CASCADE
);

CREATE TRIGGER au_predicate_definition_published_immutable
BEFORE UPDATE ON au_predicate_definition_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_published_delete_immutable
BEFORE DELETE ON au_predicate_definition_revision_v2
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_child_insert_immutable
BEFORE INSERT ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_child_update_immutable
BEFORE UPDATE ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_child_delete_immutable
BEFORE DELETE ON au_predicate_witness_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_node_insert_immutable
BEFORE INSERT ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_node_update_immutable
BEFORE UPDATE ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_node_delete_immutable
BEFORE DELETE ON au_predicate_expression_node_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_edge_insert_immutable
BEFORE INSERT ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=NEW.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_edge_update_immutable
BEFORE UPDATE ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;
CREATE TRIGGER au_predicate_definition_edge_delete_immutable
BEFORE DELETE ON au_predicate_expression_edge_v2
WHEN (SELECT revision_state FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=OLD.predicate_definition_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate definition revisions are immutable'); END;

CREATE TRIGGER au_predicate_execution_binding_published_immutable
BEFORE UPDATE ON au_predicate_execution_binding_revision
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate execution binding revisions are immutable'); END;
CREATE TRIGGER au_predicate_execution_binding_published_delete_immutable
BEFORE DELETE ON au_predicate_execution_binding_revision
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate execution binding revisions are immutable'); END;
CREATE TRIGGER au_predicate_execution_binding_source_published_insert_immutable
BEFORE INSERT ON au_predicate_execution_binding_witness_source
WHEN (SELECT revision_state FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_revision_id=NEW.predicate_execution_binding_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate execution binding revisions are immutable'); END;
CREATE TRIGGER au_predicate_execution_binding_source_published_update_immutable
BEFORE UPDATE ON au_predicate_execution_binding_witness_source
WHEN (SELECT revision_state FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_revision_id=OLD.predicate_execution_binding_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate execution binding revisions are immutable'); END;
CREATE TRIGGER au_predicate_execution_binding_source_published_delete_immutable
BEFORE DELETE ON au_predicate_execution_binding_witness_source
WHEN (SELECT revision_state FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_revision_id=OLD.predicate_execution_binding_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate execution binding revisions are immutable'); END;

CREATE TRIGGER au_predicate_group_published_immutable
BEFORE UPDATE ON au_predicate_group_revision
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_published_delete_immutable
BEFORE DELETE ON au_predicate_group_revision
WHEN OLD.revision_state='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_member_published_insert_immutable
BEFORE INSERT ON au_predicate_group_member
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=NEW.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_member_published_update_immutable
BEFORE UPDATE ON au_predicate_group_member
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=OLD.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_member_published_delete_immutable
BEFORE DELETE ON au_predicate_group_member
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=OLD.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_hook_published_insert_immutable
BEFORE INSERT ON au_predicate_group_member_hook
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=NEW.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_hook_published_update_immutable
BEFORE UPDATE ON au_predicate_group_member_hook
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=OLD.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;
CREATE TRIGGER au_predicate_group_hook_published_delete_immutable
BEFORE DELETE ON au_predicate_group_member_hook
WHEN (SELECT revision_state FROM au_predicate_group_revision WHERE predicate_group_revision_id=OLD.predicate_group_revision_id)='PUBLISHED'
BEGIN SELECT RAISE(ABORT,'published predicate group revisions are immutable'); END;

CREATE INDEX ix_au_predicate_execution_binding_revision_state
ON au_predicate_execution_binding_revision(revision_state,predicate_execution_binding_revision_id DESC);
CREATE INDEX ix_au_predicate_group_revision_state
ON au_predicate_group_revision(revision_state,predicate_group_revision_id DESC);

COMMIT;
