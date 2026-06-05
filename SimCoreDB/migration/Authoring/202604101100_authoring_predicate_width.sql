ALTER TABLE au_predicate_spec
    ADD COLUMN width INTEGER NOT NULL DEFAULT 4 CHECK(width IN (1, 2, 4, 8));
