PRAGMA foreign_keys = OFF;
PRAGMA legacy_alter_table = ON;
BEGIN IMMEDIATE;

ALTER TABLE state_artifact RENAME TO state_artifact_before_battle_context;

CREATE TABLE state_artifact (
    artifact_id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL CHECK(size_bytes >= 0),
    compression_kind INTEGER NOT NULL,
    filename TEXT NOT NULL,
    file_ext TEXT NOT NULL,
    artifact_kind TEXT NOT NULL CHECK(artifact_kind IN
        ('DTM', 'DTMINI', 'TAS_MOVIE_ITINERARY', 'SAV', 'LOG',
         'BATTLE_CONTEXT', 'OTHER')),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_state_artifact_sha256 UNIQUE (sha256)
);

INSERT INTO state_artifact(
    artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,
    artifact_kind,created_at_utc)
SELECT artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,
       artifact_kind,created_at_utc
FROM state_artifact_before_battle_context;

DROP TABLE state_artifact_before_battle_context;

COMMIT;
PRAGMA legacy_alter_table = OFF;
PRAGMA foreign_keys = ON;
