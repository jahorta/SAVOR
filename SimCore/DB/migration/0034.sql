-- Migration 0034: predicate multi-breakpoint support
BEGIN;

ALTER TABLE predicate_spec ADD COLUMN required_bp_multi TEXT;

COMMIT;
