BEGIN IMMEDIATE;

ALTER TABLE ui_projection_dead_letter ADD COLUMN payload_ref_kind TEXT NOT NULL DEFAULT '';
ALTER TABLE ui_projection_dead_letter ADD COLUMN payload_ref_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_dead_letter ADD COLUMN is_dead_letter INTEGER NOT NULL DEFAULT 1;
ALTER TABLE ui_projection_dead_letter ADD COLUMN failure_count INTEGER NOT NULL DEFAULT 0;

COMMIT;
