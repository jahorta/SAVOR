BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS ui_projection_dirty_entity (
    stream_id TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    entity_kind TEXT NOT NULL,
    entity_id INTEGER NOT NULL,
    first_outbox_id INTEGER NOT NULL,
    last_outbox_id INTEGER NOT NULL,
    event_count INTEGER NOT NULL DEFAULT 0,
    updated_at_utc INTEGER NOT NULL,
    PRIMARY KEY (stream_id, entity_kind, entity_id)
);

CREATE INDEX IF NOT EXISTS ix_ui_projection_dirty_entity_kind_outbox
    ON ui_projection_dirty_entity(stream_id, entity_kind, last_outbox_id);

CREATE INDEX IF NOT EXISTS ix_ui_projection_dirty_entity_updated
    ON ui_projection_dirty_entity(stream_id, updated_at_utc);

COMMIT;
