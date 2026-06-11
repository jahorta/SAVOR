BEGIN IMMEDIATE;

ALTER TABLE ui_projection_subscription ADD COLUMN stream_id TEXT NOT NULL DEFAULT '';
ALTER TABLE ui_projection_subscription ADD COLUMN source_high_water_outbox_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN lag_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN lag_age_ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN last_batch_size INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN last_run_duration_ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN consecutive_failures INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_projection_subscription ADD COLUMN dead_letter_count INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS ui_projection_dead_letter (
    dead_letter_id INTEGER PRIMARY KEY,
    stream_id TEXT NOT NULL,
    projector_name TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    outbox_id INTEGER NOT NULL,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    event_version INTEGER NOT NULL,
    error_text TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS ux_ui_projection_dead_letter_stream_outbox
    ON ui_projection_dead_letter(stream_id, outbox_id);

CREATE INDEX IF NOT EXISTS ix_ui_projection_subscription_stream_lag
    ON ui_projection_subscription(stream_id, status, lag_count, lag_age_ms);

COMMIT;
