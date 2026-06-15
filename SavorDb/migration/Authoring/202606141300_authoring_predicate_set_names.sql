ALTER TABLE au_predicate_set ADD COLUMN name TEXT NULL;

UPDATE au_predicate_set
SET name = 'Predicate Set ' || predicate_set_id
WHERE name IS NULL OR trim(name) = '';

CREATE UNIQUE INDEX IF NOT EXISTS uq_au_predicate_set_name
ON au_predicate_set(name);
