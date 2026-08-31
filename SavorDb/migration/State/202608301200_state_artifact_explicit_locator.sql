PRAGMA foreign_keys = OFF;
PRAGMA legacy_alter_table = ON;
BEGIN IMMEDIATE;

ALTER TABLE state_artifact RENAME TO state_artifact_before_explicit_locator;

CREATE TABLE state_artifact (
    artifact_id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL CHECK(size_bytes >= 0),
    compression_kind INTEGER NOT NULL,
    display_filename TEXT NOT NULL,
    file_ext TEXT NOT NULL,
    artifact_kind TEXT NOT NULL CHECK(artifact_kind IN
        ('DTM', 'DTMINI', 'TAS_MOVIE_ITINERARY', 'SAV', 'LOG',
         'BATTLE_CONTEXT', 'BATTLE_COMPLETION',
         'TAS_MOVIE_INPUT_EPOCH_SCHEDULE', 'OTHER')),
    created_at_utc INTEGER NOT NULL,
    object_relpath TEXT NOT NULL CHECK(length(object_relpath) > 0),
    CONSTRAINT uq_state_artifact_sha256 UNIQUE (sha256)
);

INSERT INTO state_artifact(
    artifact_id,sha256,size_bytes,compression_kind,display_filename,file_ext,
    artifact_kind,created_at_utc,object_relpath)
SELECT artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,
       artifact_kind,created_at_utc,object_relpath
FROM state_artifact_before_explicit_locator;

DROP TABLE state_artifact_before_explicit_locator;

COMMIT;
PRAGMA legacy_alter_table = OFF;
PRAGMA foreign_keys = ON;
