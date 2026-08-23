-- Hard cut: legacy automatic CANCELED records and v1 durable terminal blobs
-- are not recovered by this runtime.
ALTER TABLE exec_job_cancellation_request
    ADD COLUMN terminal_disposition TEXT NOT NULL DEFAULT 'USER_WORKFLOW_CANCEL'
    CHECK(terminal_disposition IN (
        'USER_WORKFLOW_CANCEL',
        'AUTOMATIC_SUPERSESSION',
        'AUTOMATIC_FAILURE_CASCADE'
    ));
