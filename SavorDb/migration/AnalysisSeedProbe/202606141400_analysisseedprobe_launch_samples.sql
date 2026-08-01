BEGIN IMMEDIATE;

-- launch_samples_per_axis is part of the destructive base schema. This
-- retained migration is intentionally idempotent for catalog stability.
SELECT 1;

COMMIT;
