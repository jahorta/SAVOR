-- Migration 0003: battle plans + predicates

BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (3, strftime('%s','now'));

-- Concrete plan header
CREATE TABLE IF NOT EXISTS battle_plan (
  plan_id      INTEGER PRIMARY KEY,
  settings_id  INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  name         TEXT,
  fingerprint  TEXT NOT NULL UNIQUE,
  num_turns    INTEGER NOT NULL,
  created_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS idx_battle_plan_by_settings ON battle_plan(settings_id);

-- Battle plan atoms
CREATE TABLE battle_plan_atom (
  id INTEGER PRIMARY KEY,
  wire_version INTEGER NOT NULL,
  wire_bytes BLOB NOT NULL,
  action_type INTEGER,
  actor_slot INTEGER,
  is_prelude INTEGER,
  param_item_id INTEGER,
  target_slot INTEGER,
  UNIQUE (wire_version, wire_bytes)
);
CREATE INDEX ix_battle_plan_atom_type_actor
  ON battle_plan_atom(action_type, actor_slot);

-- Turns under explorer_settings
CREATE TABLE battle_plan_turn (
  plan_id       INTEGER NOT NULL REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  turn_index    INTEGER NOT NULL,
  fake_atk_count INTEGER NOT NULL,
  PRIMARY KEY(plan_id, turn_index)
);

CREATE TABLE battle_plan_turn_actor (
  plan_id     INTEGER NOT NULL REFERENCES battle_plan(plan_id) ON DELETE CASCADE,
  turn_index  INTEGER NOT NULL,
  actor_index INTEGER NOT NULL,
  atom_id     INTEGER NOT NULL REFERENCES battle_plan_atom(id),
  PRIMARY KEY(plan_id, turn_index, actor_index)
);

-- Address programs
CREATE TABLE address_program (
  id INTEGER PRIMARY KEY,
  program_version INTEGER NOT NULL,
  prog_bytes BLOB NOT NULL,
  derived_buffer_version INTEGER,
  derived_buffer_schema_hash TEXT,
  soa_structs_hash TEXT,
  description TEXT
);
CREATE UNIQUE INDEX ux_address_program_nk
  ON address_program(program_version, prog_bytes,
                     COALESCE(derived_buffer_version,0),
                     COALESCE(derived_buffer_schema_hash,''),
                     COALESCE(soa_structs_hash,''));

-- Predicate specs
CREATE TABLE predicate_spec (
  id INTEGER PRIMARY KEY,
  spec_version INTEGER NOT NULL,
  required_bp INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  width INTEGER NOT NULL,
  cmp_op INTEGER NOT NULL,
  flags INTEGER NOT NULL,
  lhs_addr INTEGER NOT NULL,
  lhs_key INTEGER,
  rhs_value INTEGER NOT NULL,
  rhs_key INTEGER,
  turn_mask INTEGER NOT NULL,
  lhs_prog_id INTEGER REFERENCES address_program(id),
  rhs_prog_id INTEGER REFERENCES address_program(id),
  desc TEXT
);
CREATE INDEX ix_predicate_spec_bp   ON predicate_spec(required_bp);

-- Explorer settings <-> predicate linkage
CREATE TABLE explorer_settings_predicate (
  settings_id  INTEGER NOT NULL REFERENCES explorer_settings(id) ON DELETE CASCADE,
  ordinal      INTEGER NOT NULL,
  predicate_id INTEGER NOT NULL REFERENCES predicate_spec(id),
  PRIMARY KEY (settings_id, ordinal)
);
CREATE INDEX ix_settings_predicate_pid
  ON explorer_settings_predicate(settings_id, predicate_id);

  -- Explorer run: store final INI and a reference to the concatenated progress log object
ALTER TABLE explorer_run ADD COLUMN results_ini TEXT;
ALTER TABLE explorer_run ADD COLUMN progress_log_artifact_id INTEGER;

COMMIT;
