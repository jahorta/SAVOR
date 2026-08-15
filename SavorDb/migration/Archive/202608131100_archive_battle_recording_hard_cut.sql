BEGIN IMMEDIATE;

DELETE FROM ar_archive_item_kind_catalog
WHERE item_kind = 'analysis_battle_results';

INSERT INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES(
    'analysis_battle_recordings',
    1,
    'Battle recording aggregate rows reachable from archived workflows')
ON CONFLICT(item_kind) DO UPDATE SET
    include_by_default = excluded.include_by_default,
    notes = excluded.notes;

COMMIT;
