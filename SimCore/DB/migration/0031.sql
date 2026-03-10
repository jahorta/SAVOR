-- 0031.sql
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (31, strftime('%s','now'));

CREATE TABLE jobs__new (
    job_id           INTEGER PRIMARY KEY,
    job_set_id       INTEGER NOT NULL REFERENCES job_sets(job_set_id),
    program_kind     INTEGER NOT NULL REFERENCES program_kinds(kind_id),
    program_version  INTEGER NOT NULL,
    program_ref_id   INTEGER NOT NULL,
    fingerprint      TEXT    NOT NULL UNIQUE,
    priority         INTEGER NOT NULL DEFAULT 0,
    state            TEXT    NOT NULL CHECK(state IN
                       ('QUEUED','INTERRUPTED','CLAIMED','RUNNING','SUCCEEDED','FAILED',
                        'CANCELED','SUPERSEDED','SUCCEEDED_WINNER','SUCCEEDED_DUPLICATE')),
    attempts         INTEGER NOT NULL DEFAULT 0,
    max_attempts     INTEGER NOT NULL DEFAULT 5,
    claimed_by_token TEXT    NULL,
    lease_expires_at INTEGER NULL,
    queued_at        INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    vm_kv            TEXT    NULL,
    savestate_id     INTEGER REFERENCES savestate(id)
);

INSERT INTO jobs__new(
    job_id, job_set_id, program_kind, program_version, program_ref_id,
    fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at, queued_at, vm_kv, savestate_id
)
SELECT
    job_id, job_set_id, program_kind, program_version, program_ref_id,
    fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at, queued_at, vm_kv, savestate_id
FROM jobs;

DROP TABLE jobs;
ALTER TABLE jobs__new RENAME TO jobs;

CREATE INDEX IF NOT EXISTS ix_jobs_queue   ON jobs(state, program_kind, queued_at DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_lease   ON jobs(lease_expires_at);
CREATE INDEX IF NOT EXISTS ix_jobs_token   ON jobs(claimed_by_token);
CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset ON jobs(job_set_id);
CREATE INDEX IF NOT EXISTS ix_jobs_by_jobset_state_queued ON jobs(job_set_id, state, queued_at DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_queued_id_desc ON jobs(queued_at DESC, job_id DESC);
CREATE INDEX IF NOT EXISTS ix_jobs_savestate ON jobs(savestate_id);
CREATE INDEX IF NOT EXISTS ix_jobs_state_savestate_queued ON jobs(state, savestate_id, queued_at DESC);

DROP VIEW IF EXISTS v_job_queue_ready;
CREATE VIEW v_job_queue_ready AS
SELECT * FROM jobs WHERE state IN ('QUEUED','INTERRUPTED');

PRAGMA foreign_keys=ON;
COMMIT;
