BEGIN IMMEDIATE;

ALTER TABLE ui_seed_probe_summary ADD COLUMN entry_savestate_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_seed_probe_summary ADD COLUMN seed_probe_spec_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE ui_seed_probe_summary ADD COLUMN codec_version INTEGER NOT NULL DEFAULT 0;

COMMIT;
