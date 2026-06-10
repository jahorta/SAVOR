BEGIN IMMEDIATE;

DROP INDEX IF EXISTS ix_ui_projection_checkpoint_outbox;
DROP TABLE IF EXISTS ui_projection_checkpoint;

-- Validation query #1 (should return 0 rows):
-- SELECT name
-- FROM sqlite_master
-- WHERE type = 'table' AND name = 'ui_projection_checkpoint';

-- Validation query #2 (should return 0 rows):
-- SELECT name
-- FROM sqlite_master
-- WHERE type = 'index' AND name = 'ix_ui_projection_checkpoint_outbox';

-- Validation query #3 (cursor progress source of truth should be subscriptions):
-- SELECT projector_name, source_context, source_outbox_table, last_outbox_id, last_event_id, status
-- FROM ui_projection_subscription
-- ORDER BY source_context, source_outbox_table, projector_name;

COMMIT;
