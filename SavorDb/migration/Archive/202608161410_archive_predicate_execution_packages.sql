BEGIN IMMEDIATE;

INSERT INTO ar_archive_item_kind_catalog(item_kind,include_by_default,notes)
VALUES(
    'analysis_predicate_execution_packages',
    1,
    'Exact immutable Predicate Execution Packages bound to archived Battle waves'
)
ON CONFLICT(item_kind) DO UPDATE SET
    include_by_default=excluded.include_by_default,
    notes=excluded.notes;

COMMIT;
