-- 0038: global tags registry + polymorphic entity tag links
PRAGMA foreign_keys=OFF;
BEGIN;

DELETE FROM schema_version;
INSERT INTO schema_version(version, applied_at) VALUES (38, strftime('%s','now'));

CREATE TABLE IF NOT EXISTS tags (
    tag_id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_key TEXT NOT NULL UNIQUE,
    namespace TEXT NOT NULL,
    leaf_name TEXT NOT NULL,
    parent_tag_id INTEGER NULL REFERENCES tags(tag_id) ON DELETE SET NULL,
    description TEXT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    CHECK (length(trim(tag_key)) > 0),
    CHECK (length(trim(namespace)) > 0),
    CHECK (length(trim(leaf_name)) > 0),
    CHECK (instr(substr(tag_key, instr(tag_key, '::') + 2), '::') = 0)
);

CREATE INDEX IF NOT EXISTS ix_tags_parent_name ON tags(parent_tag_id, leaf_name);
CREATE INDEX IF NOT EXISTS ix_tags_namespace_name ON tags(namespace, leaf_name);

CREATE TABLE IF NOT EXISTS entity_tags (
    entity_kind TEXT NOT NULL,
    entity_id INTEGER NOT NULL,
    tag_id INTEGER NOT NULL REFERENCES tags(tag_id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    created_by TEXT NULL,
    PRIMARY KEY(entity_kind, entity_id, tag_id),
    CHECK(length(trim(entity_kind)) > 0)
);

CREATE INDEX IF NOT EXISTS ix_entity_tags_tag_kind_entity ON entity_tags(tag_id, entity_kind, entity_id);
CREATE INDEX IF NOT EXISTS ix_entity_tags_kind_entity ON entity_tags(entity_kind, entity_id);

PRAGMA foreign_keys=ON;
COMMIT;
