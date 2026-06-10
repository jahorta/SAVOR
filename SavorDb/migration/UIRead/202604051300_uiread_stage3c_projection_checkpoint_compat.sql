BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS ui_projector_checkpoint (
    projector_name TEXT PRIMARY KEY,
    last_outbox_message_id INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

UPDATE ui_projection_checkpoint
SET
    last_outbox_id = (
        SELECT legacy.last_outbox_message_id
        FROM ui_projector_checkpoint legacy
        WHERE legacy.projector_name = ui_projection_checkpoint.projector_name
    ),
    updated_at_utc = (
        SELECT legacy.updated_at_utc
        FROM ui_projector_checkpoint legacy
        WHERE legacy.projector_name = ui_projection_checkpoint.projector_name
    )
WHERE projector_name IN (
    SELECT projector_name
    FROM ui_projector_checkpoint
);

INSERT INTO ui_projection_checkpoint(projector_name, last_event_id, last_outbox_id, updated_at_utc)
SELECT legacy.projector_name, NULL, legacy.last_outbox_message_id, legacy.updated_at_utc
FROM ui_projector_checkpoint legacy
WHERE NOT EXISTS (
    SELECT 1
    FROM ui_projection_checkpoint current
    WHERE current.projector_name = legacy.projector_name
);

DROP TABLE IF EXISTS ui_projector_checkpoint;

CREATE TABLE IF NOT EXISTS ui_projection_handled_event (
    projector_name TEXT NOT NULL,
    event_id TEXT NOT NULL,
    last_outbox_id INTEGER NOT NULL,
    handled_at_utc INTEGER NOT NULL,
    PRIMARY KEY (projector_name, event_id)
);

CREATE INDEX IF NOT EXISTS ix_ui_projection_handled_event_outbox
    ON ui_projection_handled_event(projector_name, last_outbox_id);

COMMIT;
