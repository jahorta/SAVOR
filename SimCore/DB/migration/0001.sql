-- Migration 0001: initial schema

CREATE TABLE IF NOT EXISTS schema_version (
  version    INTEGER NOT NULL,
  applied_at INTEGER NOT NULL
);
INSERT INTO schema_version(version, applied_at)
VALUES (1, strftime('%s','now'));

-- Version stamps (to track build/commit versions)
CREATE TABLE version_stamp (
  id INTEGER PRIMARY KEY,
  dolphin_build_hash TEXT NOT NULL,
  wrapper_commit     TEXT NOT NULL,
  simcore_commit     TEXT NOT NULL,
  vm_opcode_hash     TEXT NOT NULL
);

-- Content-addressed objects
CREATE TABLE object_ref (
  id INTEGER PRIMARY KEY,
  sha256      TEXT NOT NULL UNIQUE,
  compression INTEGER NOT NULL,
  size        INTEGER NOT NULL,
  filename    TEXT,
  created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- Savestates
CREATE TABLE savestate (
  id INTEGER PRIMARY KEY,
  savestate_type INTEGER NOT NULL, -- 0 battle, 1 overworld
  note TEXT,
  object_ref_id INTEGER REFERENCES object_ref(id),
  complete INTEGER NOT NULL DEFAULT 0
);

-- Seed probes
CREATE TABLE seed_probe (
  id INTEGER PRIMARY KEY,
  savestate_id INTEGER NOT NULL REFERENCES savestate(id),
  version_id   INTEGER NOT NULL REFERENCES version_stamp(id),
  neutral_seed INTEGER,
  status       TEXT NOT NULL DEFAULT 'planned',
  complete     INTEGER NOT NULL DEFAULT 0
);

-- Seed deltas
CREATE TABLE seed_delta (
  id INTEGER PRIMARY KEY,
  probe_id   INTEGER NOT NULL REFERENCES seed_probe(id),
  seed_delta INTEGER NOT NULL,
  input      BLOB NOT NULL,  -- GCInputFrame as a blob
  is_grid    INTEGER NOT NULL,
  is_unique  INTEGER NOT NULL,
  complete   INTEGER NOT NULL DEFAULT 0
);

-- Explorer runs
CREATE TABLE explorer_run (
  id INTEGER PRIMARY KEY,
  probe_id INTEGER NOT NULL REFERENCES seed_probe(id),
  delta_id INTEGER NOT NULL REFERENCES seed_delta(id),
  settings_id INTEGER NOT NULL,
  progress_log TEXT,
  complete INTEGER NOT NULL DEFAULT 0
);

-- Explorer settings (metadata for runs)
CREATE TABLE explorer_settings (
  id INTEGER PRIMARY KEY,
  name TEXT,
  description TEXT,
  fingerprint TEXT
);
