BEGIN IMMEDIATE;

INSERT OR IGNORE INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES
    ('analysis_battle_completions', 1, 'Battle completion aggregate rows reachable from archived workflows'),
    ('analysis_battle_results', 1, 'Battle results aggregate rows and direct selected-seed lineage reachable from archived workflows');

COMMIT;
