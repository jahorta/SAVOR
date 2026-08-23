CREATE TABLE exec_result_staging_cleanup (
    cleanup_id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL REFERENCES exec_job(job_id) ON DELETE CASCADE,
    terminal_sha256 TEXT NOT NULL CHECK(
        length(terminal_sha256) = 64
        AND terminal_sha256 NOT GLOB '*[^0-9A-Fa-f]*'),
    program_kind INTEGER NOT NULL,
    relative_path TEXT NOT NULL CHECK(
        length(relative_path) > 0
        AND relative_path NOT LIKE '/%'
        AND relative_path NOT LIKE '\%'
        AND relative_path NOT GLOB '[A-Za-z]:*'
        AND relative_path NOT LIKE '%:%'
        AND relative_path NOT LIKE '%\%'
        AND relative_path NOT LIKE '%//%'
        AND relative_path NOT LIKE '%/./%'
        AND relative_path NOT LIKE '%/.'
        AND relative_path <> '..'
        AND relative_path NOT LIKE '../%'
        AND relative_path NOT LIKE '%/../%'
        AND relative_path NOT LIKE '%/..'),
    expected_sha256 TEXT NOT NULL CHECK(
        length(expected_sha256) = 64
        AND expected_sha256 NOT GLOB '*[^0-9A-Fa-f]*'),
    expected_size_bytes INTEGER NOT NULL CHECK(expected_size_bytes > 0),
    cleanup_state TEXT NOT NULL DEFAULT 'PENDING'
        CHECK(cleanup_state IN ('PENDING','BLOCKED')),
    cleanup_claim_token TEXT NULL,
    cleanup_lease_expires_at_utc INTEGER NULL,
    cleanup_attempts INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_attempts >= 0),
    last_error TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    blocked_at_utc INTEGER NULL,
    UNIQUE(job_id, terminal_sha256, relative_path),
    CHECK((cleanup_claim_token IS NULL AND cleanup_lease_expires_at_utc IS NULL)
       OR (cleanup_state='PENDING' AND cleanup_claim_token IS NOT NULL
           AND cleanup_lease_expires_at_utc IS NOT NULL)),
    CHECK((cleanup_state='PENDING' AND blocked_at_utc IS NULL)
       OR (cleanup_state='BLOCKED' AND blocked_at_utc IS NOT NULL))
);

CREATE INDEX ix_exec_result_staging_cleanup_claim
    ON exec_result_staging_cleanup(
        cleanup_state, cleanup_lease_expires_at_utc, cleanup_id);

CREATE INDEX ix_exec_result_staging_cleanup_job
    ON exec_result_staging_cleanup(job_id, cleanup_id);
