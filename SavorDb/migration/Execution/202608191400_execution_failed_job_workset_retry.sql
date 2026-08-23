BEGIN IMMEDIATE;

DROP TRIGGER IF EXISTS trg_exec_job_workset_membership_update;

CREATE TRIGGER trg_exec_job_workset_membership_update
BEFORE UPDATE OF workset_id, workset_item_ordinal ON exec_job
WHEN
    (
        OLD.workset_id IS NOT NULL
        AND (
            NEW.workset_id IS NOT OLD.workset_id
            OR NEW.workset_item_ordinal IS NOT OLD.workset_item_ordinal
        )
        AND NOT (
            NEW.state = 'QUEUED'
            AND NEW.workset_id IS NOT NULL
            AND NEW.workset_item_ordinal IS NOT NULL
            AND NEW.claimed_by_token IS NULL
            AND NEW.lease_expires_at_utc IS NULL
            AND NEW.dispatch_attempt_id IS NULL
            AND NEW.reserved_attempt_id IS NULL
            AND (
                (OLD.state IN ('FAILED','INTERRUPTED')
                 AND OLD.result_processing_state='PROCESSED'
                 AND NEW.max_attempts=OLD.attempts+1)
                OR (OLD.state='CLAIMED'
                    AND NEW.max_attempts=OLD.max_attempts)
                OR (OLD.state='EXECUTION_FINISHED'
                    AND NEW.result_processing_state='PROCESSED')
            )
        )
    )
    OR ((NEW.workset_id IS NULL) <> (NEW.workset_item_ordinal IS NULL))
    OR (
        NEW.workset_id IS NOT NULL
        AND NOT EXISTS (
            SELECT 1
            FROM exec_workset w
            WHERE w.workset_id = NEW.workset_id
              AND w.job_set_id = NEW.job_set_id
              AND w.program_kind = NEW.program_kind
              AND w.program_version = NEW.program_version
        )
    )
BEGIN
    SELECT RAISE(ABORT, 'immutable or invalid exec_job workset membership');
END;

COMMIT;
