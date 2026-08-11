BEGIN IMMEDIATE;

CREATE TABLE ab_predicate_bundle_binding_v2 (
    predicate_bundle_binding_id INTEGER PRIMARY KEY,
    wave_id INTEGER NULL,
    predicate_bundle_revision_id INTEGER NOT NULL,
    bundle_content_sha256 TEXT NOT NULL CHECK(length(bundle_content_sha256)=64),
    binding_content_sha256 TEXT NOT NULL CHECK(length(binding_content_sha256)=64),
    structural_active_check_sha256 TEXT NOT NULL CHECK(length(structural_active_check_sha256)=64),
    aggregation_kind TEXT NOT NULL,
    phase_program_kind INTEGER NOT NULL,
    phase_program_version INTEGER NOT NULL,
    phase_canonical_id TEXT NOT NULL,
    phase_revision INTEGER NOT NULL CHECK(phase_revision > 0),
    phase_sha256 TEXT NOT NULL CHECK(length(phase_sha256)=64),
    hook_contract_canonical_id TEXT NOT NULL,
    hook_contract_revision INTEGER NOT NULL CHECK(hook_contract_revision > 0),
    hook_contract_sha256 TEXT NOT NULL CHECK(length(hook_contract_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id),
    UNIQUE(wave_id)
);

CREATE TABLE ab_predicate_bundle_parameter_value_v2 (
    predicate_bundle_binding_id INTEGER NOT NULL,
    parameter_ordinal INTEGER NOT NULL CHECK(parameter_ordinal >= 0),
    value_kind TEXT NOT NULL,
    value_builtin_type INTEGER NULL,
    value_schema_canonical_id TEXT NULL,
    value_schema_revision INTEGER NULL,
    value_schema_sha256 TEXT NULL,
    integer_value INTEGER NULL,
    real_value REAL NULL,
    text_value TEXT NULL,
    blob_value BLOB NULL,
    PRIMARY KEY(predicate_bundle_binding_id, parameter_ordinal),
    FOREIGN KEY(predicate_bundle_binding_id)
        REFERENCES ab_predicate_bundle_binding_v2(predicate_bundle_binding_id)
        ON DELETE CASCADE
);

CREATE TABLE ab_predicate_bundle_active_check_v2 (
    predicate_bundle_binding_id INTEGER NOT NULL,
    check_ordinal INTEGER NOT NULL CHECK(check_ordinal >= 0),
    PRIMARY KEY(predicate_bundle_binding_id, check_ordinal),
    FOREIGN KEY(predicate_bundle_binding_id)
        REFERENCES ab_predicate_bundle_binding_v2(predicate_bundle_binding_id)
        ON DELETE CASCADE
);

COMMIT;
