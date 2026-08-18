BEGIN IMMEDIATE;

DROP TABLE IF EXISTS ab_predicate_bundle_active_check_v2;
DROP TABLE IF EXISTS ab_predicate_bundle_parameter_value_v2;
DROP TABLE IF EXISTS ab_predicate_bundle_binding_v2;

CREATE TABLE ab_predicate_execution_package_v1 (
    predicate_execution_package_id INTEGER PRIMARY KEY,
    wave_id INTEGER NULL UNIQUE,
    predicate_group_revision_id INTEGER NULL,
    predicate_group_sha256 TEXT NOT NULL CHECK(length(predicate_group_sha256)=64),
    execution_package_sha256 TEXT NOT NULL CHECK(length(execution_package_sha256)=64),
    execution_package_blob BLOB NOT NULL,
    phase_program_kind INTEGER NOT NULL,
    phase_program_version INTEGER NOT NULL,
    phase_canonical_id TEXT NOT NULL,
    phase_revision INTEGER NOT NULL CHECK(phase_revision > 0),
    phase_sha256 TEXT NOT NULL CHECK(length(phase_sha256)=64),
    hook_contract_canonical_id TEXT NOT NULL,
    hook_contract_revision INTEGER NOT NULL CHECK(hook_contract_revision > 0),
    hook_contract_sha256 TEXT NOT NULL CHECK(length(hook_contract_sha256)=64),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(wave_id) REFERENCES ab_turn_wave(wave_id)
);

ALTER TABLE ab_battle_single_turn_result_v1
    RENAME COLUMN predicate_bundle_revision_id TO predicate_group_revision_id;
ALTER TABLE ab_battle_single_turn_result_v1
    RENAME COLUMN predicate_bundle_sha256 TO predicate_group_sha256;
ALTER TABLE ab_battle_single_turn_result_v1
    RENAME COLUMN predicate_binding_sha256 TO predicate_execution_package_sha256;

-- Historical counts and evidence remain authoritative.  The old authored
-- bundle lineage does not identify a new group/package and is intentionally
-- cleared at the hard cut.
UPDATE ab_battle_single_turn_result_v1
SET predicate_group_revision_id=NULL,
    predicate_group_sha256=NULL,
    predicate_execution_package_sha256=NULL;

COMMIT;
