-- 0037: visual replay events stream
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (37, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS visual_replay_events (
    visual_event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    visual_replay_id INTEGER NOT NULL REFERENCES visual_replay_entries(visual_replay_id) ON DELETE CASCADE,
    event_kind TEXT NOT NULL,
    payload TEXT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS ix_visual_replay_events_replay_time
    ON visual_replay_events(visual_replay_id, created_at ASC, visual_event_id ASC);

PRAGMA foreign_keys=ON;
COMMIT;

