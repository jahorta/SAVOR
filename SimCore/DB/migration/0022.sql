-- 0022.sql object_ref created_at + indexes for pickers
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (22, strftime('%s','now'));

ALTER TABLE object_ref ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;
UPDATE object_ref SET created_at = (strftime('%s','now'));

CREATE INDEX IF NOT EXISTS ix_object_ref_created_at  ON object_ref(created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS ix_object_ref_filename    ON object_ref(filename);

CREATE INDEX IF NOT EXISTS ix_savestate_note         ON savestate(note);
CREATE INDEX IF NOT EXISTS ix_savestate_type         ON savestate(savestate_type);

CREATE INDEX IF NOT EXISTS ix_seed_probe_status      ON seed_probe(status);
CREATE INDEX IF NOT EXISTS ix_seed_probe_savestate   ON seed_probe(savestate_id);

CREATE INDEX IF NOT EXISTS ix_tas_movie_newrtc       ON tas_movie(new_rtc);

CREATE INDEX IF NOT EXISTS ix_brg_name_desc          ON battle_run_groups(name, description);

COMMIT;
PRAGMA foreign_keys=ON;