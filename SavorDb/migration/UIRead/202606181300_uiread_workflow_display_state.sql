BEGIN IMMEDIATE;

ALTER TABLE ui_workflow_instance ADD COLUMN display_state TEXT NOT NULL DEFAULT 'WAITING';

CREATE INDEX IF NOT EXISTS ix_ui_workflow_instance_display_state_created
    ON ui_workflow_instance(display_state, created_at_utc DESC, workflow_instance_id DESC);

COMMIT;
