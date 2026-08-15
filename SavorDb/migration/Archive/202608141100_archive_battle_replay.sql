BEGIN IMMEDIATE;

INSERT INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES(
    'analysis_battle_replays',
    1,
    'Artifact-free Battle replay outcomes reachable from archived workflows')
ON CONFLICT(item_kind) DO UPDATE SET
    include_by_default = excluded.include_by_default,
    notes = excluded.notes;

COMMIT;
