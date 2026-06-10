BEGIN IMMEDIATE;

CREATE TABLE au_predicate_spec_new (
    predicate_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    breakpoint_name TEXT NOT NULL,
    breakpoint_id INTEGER NOT NULL,
    lhs_value INTEGER NOT NULL,
    rhs_value INTEGER NOT NULL,
    baseline_bps TEXT NOT NULL DEFAULT '',
    cmp_op TEXT NOT NULL,
    width INTEGER NOT NULL CHECK(width IN (1, 2, 4, 8)),
    flag_mask INTEGER NULL,
    value_mask INTEGER NULL,
    lhs_address_program_id INTEGER NULL,
    rhs_address_program_id INTEGER NULL,
    abort_on_fail INTEGER NOT NULL CHECK(abort_on_fail IN (0, 1)),
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(lhs_address_program_id) REFERENCES au_address_program(address_program_id),
    FOREIGN KEY(rhs_address_program_id) REFERENCES au_address_program(address_program_id),
    CONSTRAINT uq_au_predicate_spec_name UNIQUE (name)
);

INSERT INTO au_predicate_spec_new(
    predicate_spec_id,
    name,
    breakpoint_name,
    breakpoint_id,
    lhs_value,
    rhs_value,
    baseline_bps,
    cmp_op,
    width,
    flag_mask,
    value_mask,
    lhs_address_program_id,
    rhs_address_program_id,
    abort_on_fail,
    created_at_utc
)
SELECT
    predicate_spec_id,
    name,
    breakpoint_name,
    breakpoint_id,
    lhs_value,
    rhs_value,
    '',
    cmp_op,
    width,
    flag_mask,
    value_mask,
    lhs_address_program_id,
    rhs_address_program_id,
    abort_on_fail,
    created_at_utc
FROM au_predicate_spec;

DROP TABLE au_predicate_spec;

ALTER TABLE au_predicate_spec_new RENAME TO au_predicate_spec;

COMMIT;
