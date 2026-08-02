CREATE TABLE IF NOT EXISTS exec_work_availability (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
    generation INTEGER NOT NULL CHECK(generation > 0),
    has_ready_worksets INTEGER NOT NULL CHECK(has_ready_worksets IN (0, 1)),
    has_execution_finished_results INTEGER NOT NULL
        CHECK(has_execution_finished_results IN (0, 1)),
    changed_at_utc INTEGER NOT NULL
);

INSERT OR IGNORE INTO exec_work_availability(
    singleton_id,
    generation,
    has_ready_worksets,
    has_execution_finished_results,
    changed_at_utc
)
SELECT
    1,
    1,
    CASE WHEN EXISTS(
        SELECT 1
        FROM exec_workset w
        WHERE EXISTS(
            SELECT 1 FROM exec_job j
            WHERE j.workset_id = w.workset_id
              AND j.state = 'QUEUED'
              AND j.attempts < j.max_attempts
        )
        AND NOT EXISTS(
            SELECT 1 FROM exec_job j
            WHERE j.workset_id = w.workset_id
              AND j.state = 'QUEUED'
              AND j.attempts >= j.max_attempts
        )
        AND NOT EXISTS(
            SELECT 1 FROM exec_workset_dispatch_attempt d
            WHERE d.workset_id = w.workset_id
              AND d.state IN ('CLAIMED', 'ACTIVE', 'DRAINING')
        )
    ) THEN 1 ELSE 0 END,
    CASE WHEN EXISTS(
        SELECT 1
        FROM exec_job j
        JOIN exec_temp_blob b ON b.temp_blob_id=j.worker_result_blob_id
        WHERE j.state='EXECUTION_FINISHED'
          AND j.result_processing_state='PENDING'
          AND b.cleanup_state='LIVE'
    ) THEN 1 ELSE 0 END,
    CAST(unixepoch('now') * 1000 AS INTEGER);
