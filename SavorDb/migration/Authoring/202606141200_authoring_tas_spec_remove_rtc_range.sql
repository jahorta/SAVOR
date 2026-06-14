PRAGMA foreign_keys = OFF;

BEGIN IMMEDIATE;

CREATE TABLE au_tas_spec_new (
    tas_spec_id INTEGER PRIMARY KEY,
    tas_spec_base_id INTEGER NOT NULL,
    base_dtm_artifact_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(tas_spec_base_id) REFERENCES au_tas_spec_base(tas_spec_base_id)
);

INSERT INTO au_tas_spec_new(
    tas_spec_id,
    tas_spec_base_id,
    base_dtm_artifact_id,
    created_at_utc
)
SELECT
    tas_spec_id,
    tas_spec_base_id,
    base_dtm_artifact_id,
    created_at_utc
FROM au_tas_spec;

DROP TABLE au_tas_spec;

ALTER TABLE au_tas_spec_new RENAME TO au_tas_spec;

COMMIT;

PRAGMA foreign_keys = ON;
