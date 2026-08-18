-- Predicate semantic Battle-state acquisition is backend-planned. The
-- persisted source pins the exact derived-state query, but no longer means
-- that the query must execute at the Predicate Group's evaluation hook.

CREATE TABLE au_predicate_execution_binding_witness_source_new (
    predicate_execution_binding_revision_id INTEGER NOT NULL,
    witness_ordinal INTEGER NOT NULL CHECK(witness_ordinal >= 0),
    source_kind TEXT NOT NULL CHECK(source_kind IN ('CONCRETE_VALUE','DERIVED_STATE_QUERY','CURRENT_HOOK_RECEIPT','PINNED_GUEST_MEMORY','BASELINE_OBSERVATION')),
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

INSERT INTO au_predicate_execution_binding_witness_source_new(
    predicate_execution_binding_revision_id,witness_ordinal,source_kind,
    value_builtin_type,value_schema_canonical_id,value_schema_revision,
    value_schema_sha256,literal_kind,literal_integer,literal_real,literal_text,
    literal_blob,observation_source_kind,source_canonical_id,source_revision,
    source_sha256,source_field,pinned_guest_address,baseline_capture_hook_id,
    baseline_update_policy)
SELECT
    predicate_execution_binding_revision_id,witness_ordinal,
    CASE source_kind WHEN 'CURRENT_HOOK_QUERY' THEN 'DERIVED_STATE_QUERY' ELSE source_kind END,
    value_builtin_type,value_schema_canonical_id,value_schema_revision,
    value_schema_sha256,literal_kind,literal_integer,literal_real,literal_text,
    literal_blob,observation_source_kind,source_canonical_id,source_revision,
    source_sha256,source_field,pinned_guest_address,baseline_capture_hook_id,
    baseline_update_policy
FROM au_predicate_execution_binding_witness_source;

DROP TABLE au_predicate_execution_binding_witness_source;
ALTER TABLE au_predicate_execution_binding_witness_source_new
    RENAME TO au_predicate_execution_binding_witness_source;

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
