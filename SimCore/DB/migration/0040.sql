-- 0040: TAS frame detector outputs
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at)
VALUES (40, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS tas_frame_detect (
  id               INTEGER PRIMARY KEY,
  artifact_id      INTEGER NOT NULL REFERENCES object_refs(id) ON DELETE CASCADE,
  dtm_artifact_id  INTEGER NOT NULL REFERENCES object_refs(id) ON DELETE CASCADE,
  created_at       INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE INDEX IF NOT EXISTS ix_tas_frame_detect_artifact_id
  ON tas_frame_detect(artifact_id);

CREATE INDEX IF NOT EXISTS ix_tas_frame_detect_dtm_artifact_id
  ON tas_frame_detect(dtm_artifact_id);

PRAGMA foreign_keys=ON;
COMMIT;
