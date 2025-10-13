-- 0015.sql: Config table (single-row, column-oriented)
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (15, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS config (
  config_id INTEGER PRIMARY KEY CHECK(config_id=1),

  -- Paths
  object_store_dir TEXT,
  temp_dir TEXT,

  -- SQLite/DB behavior
  busy_timeout_ms INTEGER,
  foreign_keys INTEGER,
  synchronous TEXT,                 -- OFF|NORMAL|FULL|EXTRA
  wal_autocheckpoint_pages INTEGER,

  -- Workers / scheduling
  max_workers INTEGER,
  process_reuse INTEGER,            -- 0/1

  -- Retry / leases
  retry_initial_backoff_ms INTEGER,
  retry_backoff_multiplier_x100 INTEGER,
  retry_max_backoff_ms INTEGER,
  lease_timeout_ms INTEGER,
  heartbeat_interval_ms INTEGER,

  -- bookkeeping
  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

INSERT OR IGNORE INTO config(config_id) VALUES(1);

CREATE TRIGGER IF NOT EXISTS trg_config_updated_at
AFTER UPDATE ON config
FOR EACH ROW
BEGIN
  UPDATE config SET updated_at = CURRENT_TIMESTAMP WHERE config_id = NEW.config_id;
END;

COMMIT;
