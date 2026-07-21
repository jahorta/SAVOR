PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

CREATE TABLE sp_probe_set_field_return_v2 (
    probe_set_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    probe_flavor TEXT NOT NULL CHECK(probe_flavor IN ('BATTLE_PRE', 'DUNGEON_PRE', 'OVERWORLD_PRE', 'FIELD_RETURN')),
    breakpoint_policy_name TEXT NOT NULL,
    dungeon_segment_file_num INTEGER NULL,
    dungeon_segment_file_letter TEXT NULL,
    dungeon_segment_code TEXT NULL,
    segment_source_kind TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_sp_probe_set_name UNIQUE (name)
);

INSERT INTO sp_probe_set_field_return_v2(
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    dungeon_segment_file_num,dungeon_segment_file_letter,dungeon_segment_code,
    segment_source_kind,created_at_utc)
SELECT
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    dungeon_segment_file_num,dungeon_segment_file_letter,dungeon_segment_code,
    segment_source_kind,created_at_utc
FROM sp_probe_set;

DROP TABLE sp_probe_set;
ALTER TABLE sp_probe_set_field_return_v2 RENAME TO sp_probe_set;

COMMIT;
PRAGMA foreign_keys = ON;
