PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

DROP TABLE IF EXISTS state_tas_movie_variant;

ALTER TABLE state_savestate_derivation RENAME TO state_savestate_derivation_old;
ALTER TABLE state_savestate RENAME TO state_savestate_old;
ALTER TABLE state_artifact RENAME TO state_artifact_old;

CREATE TABLE state_artifact (
    artifact_id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL CHECK(size_bytes >= 0),
    compression_kind INTEGER NOT NULL,
    filename TEXT NOT NULL,
    file_ext TEXT NOT NULL,
    artifact_kind TEXT NOT NULL CHECK(artifact_kind IN
        ('DTM', 'DTMINI', 'TAS_MOVIE_ITINERARY', 'SAV', 'LOG', 'OTHER')),
    created_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_state_artifact_sha256 UNIQUE (sha256)
);
INSERT INTO state_artifact
SELECT artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc
FROM state_artifact_old;

CREATE TABLE state_savestate (
    savestate_id INTEGER PRIMARY KEY,
    artifact_id INTEGER NOT NULL,
    savestate_type TEXT NOT NULL,
    note TEXT NULL,
    is_complete INTEGER NOT NULL CHECK(is_complete IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(artifact_id) REFERENCES state_artifact(artifact_id),
    CONSTRAINT uq_state_savestate_artifact UNIQUE (artifact_id)
);
INSERT INTO state_savestate
SELECT savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc
FROM state_savestate_old;

CREATE TABLE state_savestate_derivation (
    derivation_id INTEGER PRIMARY KEY,
    from_savestate_id INTEGER NOT NULL,
    to_savestate_id INTEGER NOT NULL,
    method_kind TEXT NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(from_savestate_id) REFERENCES state_savestate(savestate_id),
    FOREIGN KEY(to_savestate_id) REFERENCES state_savestate(savestate_id)
);
INSERT INTO state_savestate_derivation
SELECT derivation_id,from_savestate_id,to_savestate_id,method_kind,source_context_kind,source_context_id,created_at_utc
FROM state_savestate_derivation_old;

DROP TABLE state_savestate_derivation_old;
DROP TABLE state_savestate_old;
DROP TABLE state_artifact_old;

CREATE TABLE state_tas_movie_root (
    tas_movie_root_id INTEGER PRIMARY KEY,
    source_dtm_artifact_id INTEGER NOT NULL,
    dtm_artifact_id INTEGER NOT NULL,
    rtc_value INTEGER NOT NULL CHECK(rtc_value >= 0),
    itinerary_artifact_id INTEGER NOT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL CHECK(required_final_breakpoint_pc BETWEEN 0 AND 4294967295),
    checkpoint_savestate_id INTEGER NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL CHECK(source_context_id > 0),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(source_dtm_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(dtm_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(itinerary_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(checkpoint_savestate_id) REFERENCES state_savestate(savestate_id),
    CONSTRAINT uq_state_tas_movie_root_dtm UNIQUE (dtm_artifact_id),
    CONSTRAINT uq_state_tas_movie_root_source_rtc UNIQUE (source_dtm_artifact_id, rtc_value)
);

CREATE TABLE state_tas_movie_trees (
    tas_movie_tree_id INTEGER PRIMARY KEY,
    tas_movie_root_id INTEGER NOT NULL,
    parent_tas_movie_tree_id INTEGER NULL,
    dtm_artifact_id INTEGER NOT NULL,
    itinerary_artifact_id INTEGER NOT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL CHECK(required_final_breakpoint_pc BETWEEN 0 AND 4294967295),
    checkpoint_savestate_id INTEGER NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL CHECK(source_context_id > 0),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(tas_movie_root_id) REFERENCES state_tas_movie_root(tas_movie_root_id),
    FOREIGN KEY(parent_tas_movie_tree_id) REFERENCES state_tas_movie_trees(tas_movie_tree_id),
    FOREIGN KEY(dtm_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(itinerary_artifact_id) REFERENCES state_artifact(artifact_id),
    FOREIGN KEY(checkpoint_savestate_id) REFERENCES state_savestate(savestate_id),
    CONSTRAINT uq_state_tas_movie_tree_dtm UNIQUE (dtm_artifact_id)
);

CREATE INDEX ix_state_savestate_derivation_from ON state_savestate_derivation(from_savestate_id);
CREATE INDEX ix_state_savestate_derivation_to ON state_savestate_derivation(to_savestate_id);
CREATE INDEX ix_state_savestate_derivation_source_context
    ON state_savestate_derivation(source_context_kind, source_context_id, derivation_id);
CREATE INDEX ix_state_tas_movie_trees_root_parent
    ON state_tas_movie_trees(tas_movie_root_id, parent_tas_movie_tree_id, tas_movie_tree_id);

COMMIT;
PRAGMA foreign_keys = ON;
