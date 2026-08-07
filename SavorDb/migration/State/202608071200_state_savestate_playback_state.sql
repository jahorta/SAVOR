BEGIN IMMEDIATE;

ALTER TABLE state_savestate
    ADD COLUMN playback_state TEXT NOT NULL DEFAULT 'MOVIE_INACTIVE'
    CHECK(playback_state IN ('MOVIE_INACTIVE','MOVIE_PAIRED'));
ALTER TABLE state_savestate
    ADD COLUMN dtm_artifact_id INTEGER NULL REFERENCES state_artifact(artifact_id);

UPDATE state_savestate
SET playback_state='MOVIE_PAIRED',
    dtm_artifact_id=(
        SELECT r.dtm_artifact_id
        FROM state_tas_movie_root r
        WHERE r.checkpoint_savestate_id=state_savestate.savestate_id)
WHERE savestate_id IN (
    SELECT checkpoint_savestate_id FROM state_tas_movie_root);

UPDATE state_savestate
SET playback_state='MOVIE_PAIRED',
    dtm_artifact_id=(
        SELECT t.dtm_artifact_id
        FROM state_tas_movie_trees t
        WHERE t.checkpoint_savestate_id=state_savestate.savestate_id)
WHERE savestate_id IN (
    SELECT checkpoint_savestate_id FROM state_tas_movie_trees);

CREATE TRIGGER state_savestate_playback_insert_guard
BEFORE INSERT ON state_savestate
BEGIN
    SELECT CASE
        WHEN NEW.playback_state='MOVIE_INACTIVE' AND NEW.dtm_artifact_id IS NOT NULL
            THEN RAISE(ABORT,'movie-inactive savestate cannot reference a DTM')
        WHEN NEW.playback_state='MOVIE_PAIRED' AND NEW.dtm_artifact_id IS NULL
            THEN RAISE(ABORT,'movie-paired savestate requires a DTM')
        WHEN NEW.dtm_artifact_id IS NOT NULL AND
             COALESCE((SELECT artifact_kind FROM state_artifact
                       WHERE artifact_id=NEW.dtm_artifact_id),'')<>'DTM'
            THEN RAISE(ABORT,'savestate DTM reference is not a DTM artifact')
    END;
END;

CREATE TRIGGER state_savestate_playback_update_guard
BEFORE UPDATE OF playback_state,dtm_artifact_id ON state_savestate
BEGIN
    SELECT CASE
        WHEN NEW.playback_state='MOVIE_INACTIVE' AND NEW.dtm_artifact_id IS NOT NULL
            THEN RAISE(ABORT,'movie-inactive savestate cannot reference a DTM')
        WHEN NEW.playback_state='MOVIE_PAIRED' AND NEW.dtm_artifact_id IS NULL
            THEN RAISE(ABORT,'movie-paired savestate requires a DTM')
        WHEN NEW.dtm_artifact_id IS NOT NULL AND
             COALESCE((SELECT artifact_kind FROM state_artifact
                       WHERE artifact_id=NEW.dtm_artifact_id),'')<>'DTM'
            THEN RAISE(ABORT,'savestate DTM reference is not a DTM artifact')
    END;
END;

CREATE TRIGGER state_tas_movie_root_checkpoint_guard
BEFORE INSERT ON state_tas_movie_root
BEGIN
    SELECT CASE WHEN NOT EXISTS(
        SELECT 1 FROM state_savestate s
        WHERE s.savestate_id=NEW.checkpoint_savestate_id
          AND s.playback_state='MOVIE_PAIRED'
          AND s.dtm_artifact_id=NEW.dtm_artifact_id)
        THEN RAISE(ABORT,'TAS Movie root checkpoint/DTM pairing disagrees') END;
END;

CREATE TRIGGER state_tas_movie_tree_checkpoint_guard
BEFORE INSERT ON state_tas_movie_trees
BEGIN
    SELECT CASE WHEN NOT EXISTS(
        SELECT 1 FROM state_savestate s
        WHERE s.savestate_id=NEW.checkpoint_savestate_id
          AND s.playback_state='MOVIE_PAIRED'
          AND s.dtm_artifact_id=NEW.dtm_artifact_id)
        THEN RAISE(ABORT,'TAS Movie tree checkpoint/DTM pairing disagrees') END;
END;

CREATE UNIQUE INDEX uq_state_tasmovie_sterilization_source_v1
    ON state_savestate_derivation(from_savestate_id)
    WHERE method_kind='tasmovie.checkpoint_sterilize.v1';

COMMIT;
