BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS au_battle_chain_spec (
    battle_chain_spec_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NULL,
    battle_run_spec_id INTEGER NOT NULL,
    explorer_settings_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(battle_run_spec_id) REFERENCES au_battle_run_spec(battle_run_spec_id),
    FOREIGN KEY(explorer_settings_id) REFERENCES au_explorer_settings(explorer_settings_id),
    CONSTRAINT uq_au_battle_chain_spec_name UNIQUE (name)
);

INSERT OR IGNORE INTO au_battle_chain_spec(
    battle_chain_spec_id,
    name,
    description,
    battle_run_spec_id,
    explorer_settings_id,
    created_at_utc)
SELECT
    template_id,
    name,
    description,
    battle_run_spec_id,
    explorer_settings_id,
    created_at_utc
FROM au_template
WHERE battle_run_spec_id IS NOT NULL
  AND explorer_settings_id IS NOT NULL;

UPDATE au_workflow_graph_revision_node
SET authored_ref_kind = 'authoring.battle_chain_spec'
WHERE authored_ref_kind = 'authoring.template';

DROP TABLE IF EXISTS au_template;

COMMIT;
