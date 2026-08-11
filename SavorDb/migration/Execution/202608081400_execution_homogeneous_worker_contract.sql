BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

DROP INDEX IF EXISTS ix_exec_workset_compatibility;

ALTER TABLE exec_workset
    RENAME COLUMN compatibility_key TO contract_key;

ALTER TABLE exec_workset
    ADD COLUMN program_package_sha256 TEXT NULL
        CHECK(
            program_package_sha256 IS NULL
            OR (
                length(program_package_sha256) = 64
                AND program_package_sha256 NOT GLOB '*[^0-9A-Fa-f]*'
            )
        );

ALTER TABLE exec_workset
    DROP COLUMN required_capability_mask;

-- Pre-cut worksets remain preserved with an unknown package identity. They
-- are intentionally not schedulable by the v4 reconstruction contract.
-- Every workset published after this migration must carry its exact package.
CREATE TRIGGER trg_exec_workset_program_package_insert
BEFORE INSERT ON exec_workset
WHEN NEW.program_package_sha256 IS NULL
BEGIN
    SELECT RAISE(ABORT, 'exec_workset program package identity is required');
END;

CREATE TRIGGER trg_exec_workset_program_package_update
BEFORE UPDATE OF program_package_sha256 ON exec_workset
WHEN NEW.program_package_sha256 IS NULL
BEGIN
    SELECT RAISE(ABORT, 'exec_workset program package identity is required');
END;

CREATE INDEX ix_exec_workset_dispatch_contract
    ON exec_workset(
        program_package_sha256,
        execution_affinity_key
    );

COMMIT;
