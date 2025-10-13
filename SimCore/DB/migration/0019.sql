-- 0019.sql
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (19, strftime('%s','now'));

-- add active flag
ALTER TABLE triggers ADD COLUMN active INTEGER NOT NULL DEFAULT 1;

-- convert action_kind to INTEGER FK. If your existing column is TEXT, do a rebuild.
-- Here we assume fresh DB or that you can tolerate a rebuild; otherwise do backfill/move.
CREATE TABLE triggers_new (
  trigger_id INTEGER PRIMARY KEY,
  scope TEXT NOT NULL,              -- 'job' | 'job_set'
  scope_id INTEGER NOT NULL,
  condition TEXT,                   -- INI
  action_kind INTEGER NOT NULL,     -- PK_* integer
  action_args TEXT,                 -- INI
  active INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER DEFAULT (strftime('%s','now')),
  FOREIGN KEY(action_kind) REFERENCES program_kinds(kind_id)
);

INSERT INTO triggers_new(trigger_id,scope,scope_id,condition,action_kind,action_args,active,created_at)
SELECT trigger_id,scope,scope_id,condition,
       CAST(action_kind AS INTEGER),  -- if old col was already integer text; if not, replace with mapping
       action_args,1,created_at
FROM triggers;

DROP TABLE triggers;
ALTER TABLE triggers_new RENAME TO triggers;

-- fix winners progress view to use job_sets.expected_total
DROP VIEW IF EXISTS v_winners_progress;
CREATE VIEW v_winners_progress AS
SELECT
  js.job_set_id,
  (SELECT COUNT(1) FROM seed_probe_winners w WHERE w.job_set_id = js.job_set_id) AS winners_found,
  js.expected_total AS expected_total
FROM job_sets js;

COMMIT;
