BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS au_predicate_spec_required_breakpoint (
    predicate_spec_id INTEGER NOT NULL,
    breakpoint_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    PRIMARY KEY(predicate_spec_id, ordinal),
    FOREIGN KEY(predicate_spec_id) REFERENCES au_predicate_spec(predicate_spec_id),
    CONSTRAINT uq_au_predicate_spec_required_breakpoint UNIQUE (predicate_spec_id, breakpoint_id)
);

INSERT OR IGNORE INTO au_predicate_spec_required_breakpoint(predicate_spec_id, breakpoint_id, ordinal)
SELECT predicate_spec_id, breakpoint_id, 0
FROM au_predicate_spec
WHERE breakpoint_id IS NOT NULL
  AND breakpoint_id != 0;

COMMIT;
