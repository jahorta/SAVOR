BEGIN IMMEDIATE;

CREATE INDEX IF NOT EXISTS ix_ui_projection_dirty_entity_priority
    ON ui_projection_dirty_entity(stream_id, entity_kind, updated_at_utc, last_outbox_id);

COMMIT;
