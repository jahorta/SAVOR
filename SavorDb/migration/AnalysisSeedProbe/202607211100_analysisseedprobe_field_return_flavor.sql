BEGIN IMMEDIATE;

-- FIELD_RETURN is part of the destructive base schema. This retained migration
-- is intentionally idempotent so the migration catalog remains stable.
SELECT 1;

COMMIT;
