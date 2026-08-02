BEGIN IMMEDIATE;

ALTER TABLE ui_job_detail
    ADD COLUMN result_processing_state TEXT NULL;

ALTER TABLE ui_job_detail
    ADD COLUMN result_processing_attempts INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ui_job_detail
    ADD COLUMN result_processing_failures INTEGER NOT NULL DEFAULT 0;

ALTER TABLE ui_job_detail
    ADD COLUMN result_processing_error_code TEXT NULL;

ALTER TABLE ui_job_detail
    ADD COLUMN result_processing_error_text TEXT NULL;

COMMIT;
