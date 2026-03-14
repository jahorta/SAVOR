-- 0008.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (8, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS jobs (
    job_id           INTEGER PRIMARY KEY,
    job_set_id       INTEGER NOT NULL REFERENCES job_sets(job_set_id),
    program_kind     INTEGER NOT NULL REFERENCES program_kinds(kind_id),
    program_version  INTEGER NOT NULL,
    program_ref_id   INTEGER NOT NULL,            -- SeedProbe: seed_delta.id
                                                  -- TasMovie: tas_movie.id
                                                  -- BattleRunner: explorer_run.id
    fingerprint      TEXT    NOT NULL UNIQUE,
    priority         INTEGER NOT NULL DEFAULT 0,
    state            TEXT    NOT NULL CHECK(state IN
                       ('QUEUED','CLAIMED','RUNNING','SUCCEEDED','FAILED',
                        'CANCELED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE')),
    attempts         INTEGER NOT NULL DEFAULT 0,
    max_attempts     INTEGER NOT NULL DEFAULT 5,
    claimed_by_token TEXT    NULL,
    lease_expires_at INTEGER NULL,
    queued_at        INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS ix_jobs_queue   ON jobs(state, program_kind, queued_at DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_lease   ON jobs(lease_expires_at);
CREATE INDEX IF NOT EXISTS ix_jobs_token   ON jobs(claimed_by_token);

COMMIT;
